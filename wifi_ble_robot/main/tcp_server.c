/* BSD Socket API Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include<stdint.h>
#include<stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include "sdkconfig.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "easy_crc.h"
#include "led.h"
#include "usb_uart.h"
#include "gatt_server.h"
#include "driver/gpio.h"

#define SWITCH_GPIO GPIO_NUM_18

int level=1;

#define PORT                        CONFIG_EXAMPLE_PORT
#define KEEPALIVE_IDLE              CONFIG_EXAMPLE_KEEPALIVE_IDLE
#define KEEPALIVE_INTERVAL          CONFIG_EXAMPLE_KEEPALIVE_INTERVAL
#define KEEPALIVE_COUNT             CONFIG_EXAMPLE_KEEPALIVE_COUNT

static const char *TAG = "example";

static bool connected = false;  // This is for TCP client, not Wi-Fi

#define PACKET_SIZE sizeof(JoyData)
_Static_assert(PACKET_SIZE == 17, "Unexpected packet size");

#ifdef USB
static const tinyusb_config_t tusb_cfg = {
    .device_descriptor = NULL,
    .string_descriptor = NULL,
    .external_phy = false,
    .configuration_descriptor = NULL,
};

static const tinyusb_config_cdcacm_t acm_cfg = {
    .usb_dev = TINYUSB_USBDEV_0,
    .cdc_port = TINYUSB_CDC_ACM_0,
    .rx_unread_buf_sz = 64,
    .callback_rx = NULL,
    .callback_rx_wanted_char = NULL,
    .callback_line_state_changed = NULL,
    .callback_line_coding_changed = NULL
};
#else
static const uart_config_t uart_config = {
    .baud_rate = 115200,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_DEFAULT,
};
#endif

bool led_state = false;
uint32_t freq_timer = 0;
bool freq_timer_started = false;

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

void gatt_write_recv_callback(esp_ble_gatts_cb_param_t *param)
{
  blink_led();
  memcpy(&ps4_data, param->write.value, sizeof(JoyData));
  xQueueSend(usb_uart_queue, param->write.value, 0);
}

void gatts_app(void *parameters)
{
  esp_err_t ret;

  // Initialize NVS.
  ret = nvs_flash_init();
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
    ESP_LOGE(GATTS_TAG, "%s initialize controller failed: %s", __func__, esp_err_to_name(ret));
    return;
  }

  ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (ret)
  {
    ESP_LOGE(GATTS_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
    return;
  }

  ret = esp_bluedroid_init();
  if (ret)
  {
    ESP_LOGE(GATTS_TAG, "%s init bluetooth failed: %s", __func__, esp_err_to_name(ret));
    return;
  }
  ret = esp_bluedroid_enable();
  if (ret)
  {
    ESP_LOGE(GATTS_TAG, "%s enable bluetooth failed: %s", __func__, esp_err_to_name(ret));
    return;
  }

  ret = esp_ble_gatts_register_callback(gatts_event_handler);
  if (ret)
  {
    ESP_LOGE(GATTS_TAG, "gatts register error, error code = %x", ret);
    return;
  }
  ret = esp_ble_gap_register_callback(gap_event_handler);
  if (ret)
  {
    ESP_LOGE(GATTS_TAG, "gap register error, error code = %x", ret);
    return;
  }
  ret = esp_ble_gatts_app_register(PROFILE_A_APP_ID);
  if (ret)
  {
    ESP_LOGE(GATTS_TAG, "gatts app register error, error code = %x", ret);
    return;
  }
  esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(50);
  if (local_mtu_ret)
  {
    ESP_LOGE(GATTS_TAG, "set local  MTU failed, error code = %x", local_mtu_ret);
  }

  vTaskDelete(NULL);
}

static void unpack_memcpy(const uint8_t *in_buf, JoyData *pkt) {
    memcpy(pkt, in_buf, PACKET_SIZE);
}

static void pack_memcpy(const JoyData *pkt, uint8_t *out_buf) {
    memcpy(out_buf, pkt, PACKET_SIZE);
}

static ssize_t recv_all(int sock, void *buf, size_t len, int flags) {
    uint8_t *p = buf;
    size_t recvd = 0;
    while (recvd < len) {
        ssize_t r = recv(sock, p + recvd, len - recvd, flags);
        if (r <= 0) return r;
        recvd += r;
    }
    return recvd;
}

// NEW: Event handler for Wi-Fi/IP events (add this function)
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        led_show_no_ip();  // Turn off LED on Wi-Fi disconnect
        esp_wifi_connect();  // Attempt reconnect
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        led_show_ip_acquired();  // Turn on LED when IP acquired
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        led_show_no_ip();  // Turn off LED if IP lost
    }
}

static void tcp_server_task(void *pvParameters)
{
    char addr_str[128];
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    int keepAlive = 1;
    int keepIdle = KEEPALIVE_IDLE;
    int keepInterval = KEEPALIVE_INTERVAL;
    int keepCount = KEEPALIVE_COUNT;
    struct sockaddr_storage dest_addr;

#ifdef CONFIG_EXAMPLE_IPV4
    if (addr_family == AF_INET) {
        struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
        dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr_ip4->sin_family = AF_INET;
        dest_addr_ip4->sin_port = htons(PORT);
        ip_protocol = IPPROTO_IP;
    }
#endif
#ifdef CONFIG_EXAMPLE_IPV6
    if (addr_family == AF_INET6) {
        struct sockaddr_in6 *dest_addr_ip6 = (struct sockaddr_in6 *)&dest_addr;
        bzero(&dest_addr_ip6->sin6_addr.un, sizeof(dest_addr_ip6->sin6_addr.un));
        dest_addr_ip6->sin6_family = AF_INET6;
        dest_addr_ip6->sin6_port = htons(PORT);
        ip_protocol = IPPROTO_IPV6;
    }
#endif

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#if defined(CONFIG_EXAMPLE_IPV4) && defined(CONFIG_EXAMPLE_IPV6)
    setsockopt(listen_sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
#endif

    ESP_LOGI(TAG, "Socket created");
    

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", PORT);

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }

    while (1) {

        ESP_LOGI(TAG, "Socket listening");

        struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }

        // Set tcp keepalive option
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
        
        /* Disable Nagle's algorithm for low-latency transmission */
        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(int));
        // Convert ip address to string
#ifdef CONFIG_EXAMPLE_IPV4
        if (source_addr.ss_family == PF_INET) {
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
        }
#endif
#ifdef CONFIG_EXAMPLE_IPV6
        if (source_addr.ss_family == PF_INET6) {
            inet6_ntoa_r(((struct sockaddr_in6 *)&source_addr)->sin6_addr, addr_str, sizeof(addr_str) - 1);
        }
#endif
        ESP_LOGI(TAG, "Socket accepted ip address: %s", addr_str);

        /* Mark that a client is connected so USB/UART tasks start sending data */
        connected = true;
        led_show_ip_acquired();
        uint8_t buf[PACKET_SIZE];
        uint32_t packet_count = 0;
        uint32_t crc_errors = 0;

        // Inner loop to handle multiple packets from the same client connection.
        while (1) {

            ssize_t recvd = recv_all(sock, buf, PACKET_SIZE, 0);
            blink_led();
            if (recvd <= 0) {
                /* Client disconnected or error: stop forwarding and mark disconnected */
                connected = false;
                // REMOVED: led_show_no_ip();  // Don't turn off Wi-Fi LED here; it's unrelated to Wi-Fi status
                // REMOVED: s_led_state = !s_led_state;  // Unrelated to Wi-Fi LED
                if (recvd == 0) {
                    ESP_LOGI(TAG, "Connection closed by client. Total packets: %lu, CRC errors: %lu", packet_count, crc_errors);
                } else {
                    ESP_LOGE(TAG, "Recv failed: errno %d", errno);
                }
                close(sock);
                break;  // Break inner loop to accept next connection.
            }
            
            packet_count++;
            JoyData pkt;
            unpack_memcpy(buf, &pkt);

            //uncomment this line to enable blink on packet received
            //blink_led();
            xQueueSend(usb_uart_queue, &pkt, 0);
            
            /* Send response immediately without delay */
            const char *resp = "OK";
            if (send(sock, resp, 2, 0) < 0) {
                ESP_LOGE(TAG, "Send failed: errno %d", errno);
                connected = false;
                close(sock);
                break;
            }
        }
    }

CLEAN_UP:
    /* Ensure no tasks think a client is connected when server is shutting down */
    connected = false;
    close(listen_sock);
    vTaskDelete(NULL);
}

#ifdef USB

void usb_task(void *params)
{

  uint8_t usb_out_data[sizeof(JoyData)];

  ESP_LOGI(USB_TAG, "USB initialization");

  ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

  ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm_cfg));

  ESP_LOGI(USB_TAG, "USB initialization DONE");
  while (1)
  {
    vTaskDelay(10 / portTICK_PERIOD_MS);
    if (connected)
    {
      if (xQueueReceive(usb_uart_queue, &usb_out_data, portMAX_DELAY))
      {
        ESP_LOG_BUFFER_HEXDUMP(USB_TAG, &usb_out_data, sizeof(usb_out_data), ESP_LOG_INFO);

        tinyusb_cdcacm_write_queue(0, (uint8_t *)&usb_out_data, sizeof(usb_out_data));
        esp_err_t err = tinyusb_cdcacm_write_flush(0, 0);

        blink_led();
      }
    }
  }
  vTaskDelete(NULL);
}

#else

static void uart_task(void *args)
{
  int intr_alloc_flags = 0;
  uint8_t uart_out_data[sizeof(JoyData)];

  
  ESP_ERROR_CHECK(uart_driver_install(2, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, intr_alloc_flags));
  ESP_ERROR_CHECK(uart_param_config(2, &uart_config));
  ESP_ERROR_CHECK(uart_set_pin(2, 17, 16, -1, -1));


  while (1)
  {
    vTaskDelay(10 / portTICK_PERIOD_MS);
    if (connected)
    {
      if (xQueueReceive(usb_uart_queue, &uart_out_data, portMAX_DELAY))
      {
        uart_write_bytes(2, &uart_out_data, sizeof(JoyData));
      }
    }
  }
  vTaskDelete(NULL);
}

#endif

void app_main(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SWITCH_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,  // External pull-down used
        .intr_type = GPIO_INTR_ANYEDGE
    };
    gpio_config(&io_conf);
    xTaskCreate(gpio_monitor_task, "gpio_monitor", 2048, NULL, 1, &gpio_monitor_task_handle);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(SWITCH_GPIO, gpio_isr_handler, NULL);

    level = gpio_get_level(SWITCH_GPIO);
    if (level == 1) {
            ESP_ERROR_CHECK(nvs_flash_init());
            ESP_ERROR_CHECK(esp_netif_init());
            ESP_ERROR_CHECK(esp_event_loop_create_default());

            // NEW: Register event handler for Wi-Fi and IP events (for handling disconnects)
            ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
            ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));

            esp_wifi_set_ps(WIFI_PS_NONE);

            /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
            * Read "Establishing Wi-Fi or Ethernet Connection" section in
            * examples/protocols/README.md for more information about this function.
            */
           ESP_ERROR_CHECK(example_connect());
           
           /* Show green LED to indicate IP address acquired */
           led_show_ip_acquired();
           #ifdef CONFIG_EXAMPLE_IPV4
           xTaskCreate(tcp_server_task, "tcp_server", 4096, (void*)AF_INET, 5, NULL);
           #endif
           #ifdef CONFIG_EXAMPLE_IPV6
           xTaskCreate(tcp_server_task, "tcp_server", 4096, (void*)AF_INET6, 5, NULL);
           #endif
           
        }
        else {
            xTaskCreate(gatts_app, "gatt_app", 4096, NULL, 5, NULL);
        }

        configure_led();
        usb_uart_queue = xQueueCreate(1, sizeof(JoyData));
        assert(usb_uart_queue);
#ifdef USB
      xTaskCreatePinnedToCore(usb_task, "usb task", 3072, NULL, 10, NULL, 1);
#else
      xTaskCreatePinnedToCore(uart_task, "uart_task", 3072, NULL, 10, NULL, 1);
#endif
}