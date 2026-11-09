#include <string.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include "gatt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "usb_hid.h"
#include "easy_crc.h"
#include "driver/gpio.h"


#define SWITCH_GPIO 9
#define SERVER_IP   "192.168.18.6"   // <--- change this
#define SERVER_PORT  3333
#define INCLUDE_vTaskDelete 1

int level=1;


TaskHandle_t repeated_write_handle;
static bool can_write;
static gattc_profile_inst *controller_esp_profile_inst;
void repeated_write(void *params);

static const char *TAG = "TCP_CLIENT";

#define PACKET_SIZE 17

/* Wi-Fi event group */
static EventGroupHandle_t s_wifi_event_group = NULL;
#define WIFI_CONNECTED_BIT BIT0

/* PS4 data event group (new data available) */
static EventGroupHandle_t ps4_event_group = NULL;
#define PS4_NEW_BIT BIT0

static TaskHandle_t gpio_monitor_task_handle = NULL;

// ISR Handler - runs when GPIO changes
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    
    // Notify the monitoring task that GPIO changed
    vTaskNotifyGiveFromISR(gpio_monitor_task_handle, &xHigherPriorityTaskWoken);
    
    // Request context switch if a higher priority task was woken
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

void gpio_monitor_task(void *params)
{
    int last_level = gpio_get_level(SWITCH_GPIO);
    
    while (1)
    {
        // Block indefinitely until ISR notifies us
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        // Debounce delay - wait for switch to settle
        vTaskDelay(200 / portTICK_PERIOD_MS);
        
        // Read current level after debounce
        int current_level = gpio_get_level(SWITCH_GPIO);
        
        // Verify the state actually changed (not just noise)
        if (current_level != last_level)
        {
            ESP_LOGI(TAG, "GPIO switch detected: %d -> %d, restarting...", 
                     last_level, current_level);
            esp_restart();
        }
        else
        {
            ESP_LOGW(TAG, "GPIO bounce detected, ignoring");
        }
        
        last_level = current_level;
    }
}


static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
    esp_wifi_connect();
    if (s_wifi_event_group) xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

void wifi_init_sta(void)
{
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  /* create the wifi event group used to signal when we have an IP */
  if (s_wifi_event_group == NULL) {
    s_wifi_event_group = xEventGroupCreate();
  }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* register event handlers for wifi and IP events */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "momochan",
            .password = "momochan",
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_wifi_connect());   // <-- REQUIRED

    ESP_LOGI(TAG, "WiFi init complete.");
}


// without this define the vTaskDelete wont work
 
static ps4_data_t ps4_data_in;
static JoyData ps4_data_out;
static SemaphoreHandle_t ps4_mutex = NULL;

static void pack_memcpy(const JoyData *pkt, uint8_t *out_buf) {
    memcpy(out_buf, pkt, PACKET_SIZE);
}


/* send_all is not used; TCP task sends packets directly */



void usb_hid_data_get_callback(uint8_t *data)
{
  if (ps4_mutex && xSemaphoreTake(ps4_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    memcpy(&ps4_data_in, data, sizeof(ps4_data_in));
    ps4_data_convert(&ps4_data_in, &ps4_data_out);
    xSemaphoreGive(ps4_mutex);
    if (ps4_event_group) xEventGroupSetBits(ps4_event_group, PS4_NEW_BIT);
  } else {
    /* fallback: best-effort without mutex */
    memcpy(&ps4_data_in, data, sizeof(ps4_data_in));
    ps4_data_convert(&ps4_data_in, &ps4_data_out);
    if (ps4_event_group) xEventGroupSetBits(ps4_event_group, PS4_NEW_BIT);
  }

}

void usb_hid_disconnect_callback()
{
  ESP_LOGI(TAG, "PS4 controller disconnected");
  if (ps4_mutex && xSemaphoreTake(ps4_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    memset(&ps4_data_out, 0, sizeof(JoyData));
    xSemaphoreGive(ps4_mutex);
    if (ps4_event_group) xEventGroupSetBits(ps4_event_group, PS4_NEW_BIT);
  } else {
    memset(&ps4_data_out, 0, sizeof(JoyData));
    if (ps4_event_group) xEventGroupSetBits(ps4_event_group, PS4_NEW_BIT);
  }
}

void usb_hid_task(void *parameter)
{
  BaseType_t task_created;
  app_event_queue_t evt_queue;
  ESP_LOGI(USB_TAG, "HID Host example");
  /*
   * Create usb_lib_task to:
   * - initialize USB Host library
   * - Handle USB Host events while APP pin in in HIGH state
   */
  task_created = xTaskCreatePinnedToCore(usb_lib_task,
                                         "usb_events",
                                         4096,
                                         xTaskGetCurrentTaskHandle(),
                                         2, NULL, 1);
  assert(task_created == pdTRUE);

  // Wait for notification from usb_lib_task to proceed
  ulTaskNotifyTake(false, 1000);

  /*
   * HID host driver configuration
   * - create background task for handling low level event inside the HID driver
   * - provide the device callback to get new HID Device connection event
   */
  const hid_host_driver_config_t hid_host_driver_config = {
      .create_background_task = true,
      .task_priority = 5,
      .stack_size = 4096,
      .core_id = 1,
      .callback = hid_host_device_callback,
      .callback_arg = NULL};

  ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));

  // Create queue
  app_event_queue = xQueueCreate(10, sizeof(app_event_queue_t));

  ESP_LOGI(USB_TAG, "Waiting for HID Device to be connected");

  while (1)
  {
    // Wait queue
    if (xQueueReceive(app_event_queue, &evt_queue, portMAX_DELAY))
    {
      if (APP_EVENT == evt_queue.event_group)
      {
        // User pressed button
        usb_host_lib_info_t lib_info;
        ESP_ERROR_CHECK(usb_host_lib_info(&lib_info));
        if (lib_info.num_devices == 0)
        {
          // End while cycle
          break;
        }
        else
        {
          ESP_LOGW(USB_TAG, "To shutdown example, remove all USB devices and press button again.");
          // Keep polling
        }
      }

      if (APP_EVENT_HID_HOST == evt_queue.event_group)
      {
        hid_host_device_event(evt_queue.hid_host_device.handle,
                              evt_queue.hid_host_device.event,
                              evt_queue.hid_host_device.arg);
      }
    }
  }

  ESP_LOGI(USB_TAG, "HID Driver uninstall");
  ESP_ERROR_CHECK(hid_host_uninstall());
  xQueueReset(app_event_queue);
  vQueueDelete(app_event_queue);
  vTaskDelete(NULL);
}

static void tcp_client_task(void *pvParameters)
{
    char rx_buffer[128];
    char txbuf[PACKET_SIZE];

    while (1) {
        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(SERVER_PORT);

        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        ESP_LOGI(TAG, "Socket created, connecting...");
        int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err != 0) {
            ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        ESP_LOGI(TAG, "Successfully connected");

        /* Set socket to non-blocking AFTER successful connection */
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        const TickType_t period = pdMS_TO_TICKS(33); // 30 Hz
        TickType_t last_wake = xTaskGetTickCount();

        while (1) {
            /* Wait for HID update or timeout */
            xEventGroupWaitBits(
                ps4_event_group,
                PS4_NEW_BIT,
                pdTRUE,        // clear the bit
                pdFALSE,
                period         // timeout for periodic sending
            );

            /* Prepare packet */
            JoyData local_pkt;

            if (ps4_mutex && xSemaphoreTake(ps4_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                local_pkt = ps4_data_out;
                xSemaphoreGive(ps4_mutex);
            } else {
                local_pkt = ps4_data_out;
            }

            // local_pkt.crc = calculate_cr8x_fast((uint8_t *)&local_pkt+1, 15);
            pack_memcpy(&local_pkt, (uint8_t *)txbuf);

            /* Send packet */
            int to_write = PACKET_SIZE;
            int written = send(sock, txbuf, to_write, 0);
            if (written <= 0 || written != to_write) {
                ESP_LOGE(TAG, "Send failed: errno %d", errno);
                break;  // reconnect
            }

            /* Drain all available data from the buffer */
            int total_len = 0;
            char combined_buffer[512] = {0};
            
             while (1) {
                 int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
                 if (len == 0) {
                     ESP_LOGW(TAG, "Connection closed");
                     goto connection_lost;
                 } else if (len > 0) {
                     /* Accumulate all responses */
                     if (total_len + len < sizeof(combined_buffer)) {
                         memcpy(combined_buffer + total_len, rx_buffer, len);
                         total_len += len;
                     }
                 } else {
                     /* EAGAIN/EWOULDBLOCK means no more data available */
                     if (errno == EAGAIN || errno == EWOULDBLOCK) {
                         break;  /* Done draining */
                     } else {
                         ESP_LOGE(TAG, "Recv error: errno %d", errno);
                         goto connection_lost;
                     }
                 }
             }
            
             /*Log all accumulated responses at once */
             if (total_len > 0) {
                 combined_buffer[total_len] = 0;
                 ESP_LOGI(TAG, "Received (%d bytes): %s", total_len, combined_buffer);
            }

            vTaskDelayUntil(&last_wake, period);
            continue;
            
connection_lost:
            break;
        }

        ESP_LOGW(TAG, "Closing socket and reconnecting...");
        shutdown(sock, 0);
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
void gattc_first_write_callback(gattc_profile_inst *gattc_profile)
{
  controller_esp_profile_inst = gattc_profile;
  esp_ble_gattc_write_char(controller_esp_profile_inst->gattc_if,
                           controller_esp_profile_inst->conn_id,
                           controller_esp_profile_inst->char_handle,
                           sizeof(JoyData),
                           (uint8_t *)&ps4_data_out,
                           ESP_GATT_WRITE_TYPE_RSP,
                           ESP_GATT_AUTH_REQ_NONE);
  xTaskCreatePinnedToCore(repeated_write, "repeated_write_task", 4096 * 4, NULL, 9, &repeated_write_handle, 0);
}

void gattc_write_event_callback()
{
  can_write = true;
}

void gattc_disconnect_callback()
{
  can_write = false;
  memset(&ps4_data_out, 0, sizeof(JoyData));
  if (repeated_write_handle != NULL)
  {
    vTaskDelete(repeated_write_handle);
  }
}

void repeated_write(void *params)
{
  esp_err_t error;

  while (true)
  {
    vTaskDelay(10 / portTICK_PERIOD_MS); // This delay is important, as the ble stack cant handle higher rates, and without this delay i kept getting watch dog errors
    if (can_write)
    {
      error = esp_ble_gattc_write_char(controller_esp_profile_inst->gattc_if,
                                       controller_esp_profile_inst->conn_id,
                                       controller_esp_profile_inst->char_handle,
                                       sizeof(JoyData),
                                       (uint8_t *)&ps4_data_out,
                                       ESP_GATT_WRITE_TYPE_NO_RSP,
                                       ESP_GATT_AUTH_REQ_NONE);
    };
  };
  vTaskDelete(NULL);
}

void gattc_app(void *parameters)
{
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  ret = esp_bt_controller_init(&bt_cfg);
  if (ret)
  {
    ESP_LOGE(GATTC_TAG, "%s initialize controller failed: %s", __func__, esp_err_to_name(ret));
    return;
  }

  ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (ret)
  {
    ESP_LOGE(GATTC_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
    return;
  }

  ret = esp_bluedroid_init();
  if (ret)
  {
    ESP_LOGE(GATTC_TAG, "%s init bluetooth failed: %s", __func__, esp_err_to_name(ret));
    return;
  }

  ret = esp_bluedroid_enable();
  if (ret)
  {
    ESP_LOGE(GATTC_TAG, "%s enable bluetooth failed: %s", __func__, esp_err_to_name(ret));
    return;
  }

  // register the  callback function to the gap module
  ret = esp_ble_gap_register_callback(esp_gap_cb);
  if (ret)
  {
    ESP_LOGE(GATTC_TAG, "%s gap register failed, error code = %x", __func__, ret);
    return;
  }

  // register the callback function to the gattc module
  ret = esp_ble_gattc_register_callback(esp_gattc_cb);
  if (ret)
  {
    ESP_LOGE(GATTC_TAG, "%s gattc register failed, error code = %x", __func__, ret);
    return;
  }

  ret = esp_ble_gattc_app_register(PROFILE_A_APP_ID);
  if (ret)
  {
    ESP_LOGE(GATTC_TAG, "%s gattc app register failed, error code = %x", __func__, ret);
  }
  esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(50);
  if (local_mtu_ret)
  {
    ESP_LOGE(GATTC_TAG, "set local  MTU failed, error code = %x", local_mtu_ret);
  }

  vTaskDelete(NULL);
}

void app_main(void)
{
    // Step 1: Configure GPIO with interrupt enabled
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SWITCH_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,      // ✅ Fixed: was GPIO_INTR_ANYEDGE
        .pull_down_en = GPIO_PULLDOWN_DISABLE,  // External pull-down used
        .intr_type = GPIO_INTR_ANYEDGE          // ✅ Fixed: Enable edge interrupts
    };
    gpio_config(&io_conf);
    
    // Step 2: Create monitoring task FIRST (to get handle)
    xTaskCreate(gpio_monitor_task, "gpio_monitor", 2048, NULL, 1, &gpio_monitor_task_handle);
    
    // Step 3: Install ISR service and add handler AFTER task is created
    gpio_install_isr_service(0);
    gpio_isr_handler_add(SWITCH_GPIO, gpio_isr_handler, NULL);
    
    // Step 4: Read GPIO level to determine mode
    level = gpio_get_level(SWITCH_GPIO);
    
    // Step 5: Initialize based on mode
    if (level) 
    {
        // ===== TCP/WiFi MODE =====
        ESP_LOGI(TAG, "Starting in TCP/WiFi mode");
        
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);
        
        // Disable Wi-Fi power-saving to reduce latency
        esp_wifi_set_ps(WIFI_PS_NONE);

        // Start Wi-Fi (initializes TCP/IP stack)
        wifi_init_sta();

        // Create mutex to protect ps4 data between USB callback and TCP task
        ps4_mutex = xSemaphoreCreateMutex();
        if (ps4_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create ps4 mutex");
        }

        // Create PS4 event group for new-data notifications
        if (ps4_event_group == NULL) {
            ps4_event_group = xEventGroupCreate();
        }

        xTaskCreatePinnedToCore(tcp_client_task, "tcp_client", 4096 * 2, NULL, 10, NULL, 0);
    }
    else 
    {
        // ===== BLE/GATT MODE =====
        ESP_LOGI(TAG, "Starting in BLE/GATT mode");
        
        // Create PS4 event group (needed for BLE mode too)
        if (ps4_event_group == NULL) {
            ps4_event_group = xEventGroupCreate();
        }
        
        // Note: gattc_app() will initialize NVS internally
        xTaskCreatePinnedToCore(gattc_app, "gattc_app", 4096 * 2, NULL, 10, NULL, 0);
    }
    
    // Step 6: Common initialization - USB HID task (runs in both modes)
    xTaskCreatePinnedToCore(usb_hid_task, "hid_device", 4096 * 2, NULL, 10, NULL, 1);
}