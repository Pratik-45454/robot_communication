#ifndef __USB_HID_H
#define __USB_HID_H

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "errno.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#include "usb/hid_host.h"

#include "easy_crc.h"

#define START_BYTE 0xA5
#define USB_TAG "PS4_usb"

void usb_hid_data_get_callback(uint8_t *);

void usb_hid_disconnect_callback();

QueueHandle_t app_event_queue = NULL;

/**
 * @brief APP event group
 */
typedef enum
{
  APP_EVENT = 0,
  APP_EVENT_HID_HOST
} app_event_group_t;

/**
 * @brief APP event queue
 */
typedef struct
{
  app_event_group_t event_group;
  /* HID Host - Device related info */
  struct
  {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
    void *arg;
  } hid_host_device;
} app_event_queue_t;

typedef struct __attribute__((packed))
{
  uint8_t battery : 4; // Battery level (0-15)
  uint8_t usb : 1;     // USB connection status (1 if connected)
  uint8_t audio : 1;   // Audio jack connection status (1 if connected)
  uint8_t mic : 1;     // Microphone status (1 if connected)
  uint8_t unknown : 1; // Possibly for the extension port
} Status;

/**
 * @brief PS4 data type
 */
#pragma pack(push, 1)
typedef struct
{
  uint8_t id;
  int8_t lx,
      ly,
      rx,
      ry;
  union
  {
    struct
    {
      bool dpad1 : 1;
      bool dpad2 : 1;
      bool dpad4 : 1;
      bool dpad8 : 1;
      uint8_t square : 1;
      uint8_t cross : 1;
      uint8_t circle : 1;
      uint8_t triangle : 1;

      uint8_t l1 : 1;
      uint8_t r1 : 1;
      uint8_t l2 : 1;
      uint8_t r2 : 1;
      uint8_t share : 1;
      uint8_t options : 1;
      uint8_t l3 : 1;
      uint8_t r3 : 1;

      uint8_t ps : 1;
      uint8_t touchpad : 1;
      uint8_t reportCounter : 6;
    };
    uint32_t val : 24;
  } buttons;
  uint8_t lt,
      rt;
  uint16_t ts;
  uint8_t dummy1;
  int16_t gyroY, gyroZ, gyroX;
  int16_t accX, accZ, accY;

  uint8_t dummy2[5];
  Status status;
} ps4_data_t;
#pragma pack(pop)

enum Buttons
{
  CROSS,
  CIRCLE,
  SQUARE,
  TRIANGLE,
  SHARE,
  PS4,
  OPTION,
  L3,

  R3,
  L1,
  R1,
  UP,
  DOWN,
  LEFT,
  RIGHT,
  TOUCH
};

typedef struct __attribute__((packed))
{
  uint8_t start_byte;
  int8_t lx;
  int8_t ly;
  int8_t rx;
  int8_t ry;
  uint8_t lt;
  uint8_t rt;
  uint16_t buttons;
  Status status;
  int16_t accX, accZ, accY;
  uint8_t crc;
} JoyData;

/**
 * @brief converts received ds4 data to robot data
 */
void ps4_data_convert(ps4_data_t *in_data, JoyData *out_data)
{
  out_data->start_byte = START_BYTE;

  out_data->lx = in_data->lx - 0x80;
  out_data->ly = in_data->ly - 0x80;
  out_data->rx = in_data->rx - 0x80;
  out_data->ry = in_data->ry - 0x80;
  out_data->lt = in_data->lt;
  out_data->rt = in_data->rt;

  uint16_t buttons = 0;

  buttons ^= (in_data->buttons.circle << CIRCLE);
  buttons ^= (in_data->buttons.cross << CROSS);
  buttons ^= (in_data->buttons.square << SQUARE);
  buttons ^= (in_data->buttons.triangle << TRIANGLE);
  buttons ^= (in_data->buttons.share << SHARE);
  buttons ^= (in_data->buttons.ps << PS4);
  buttons ^= (in_data->buttons.options << OPTION);
  buttons ^= (in_data->buttons.l3 << L3);

  buttons ^= (in_data->buttons.r3 << R3);
  buttons ^= (in_data->buttons.l1 << L1);
  buttons ^= (in_data->buttons.r1 << R1);
  buttons ^= (in_data->buttons.touchpad << TOUCH);

  buttons ^= (((!in_data->buttons.dpad8 & !in_data->buttons.dpad4 & !in_data->buttons.dpad2) |
               (in_data->buttons.dpad4 & in_data->buttons.dpad2 & in_data->buttons.dpad1))
              << UP);
  buttons ^= (((in_data->buttons.dpad4 & !in_data->buttons.dpad2) |
               (!in_data->buttons.dpad4 & in_data->buttons.dpad2 & in_data->buttons.dpad1))
              << DOWN);
  buttons ^= ((in_data->buttons.dpad4 & (in_data->buttons.dpad2 | in_data->buttons.dpad1)) << LEFT);
  buttons ^= (((!in_data->buttons.dpad4) & (in_data->buttons.dpad2 | in_data->buttons.dpad1)) << RIGHT);

  out_data->buttons = buttons;

  out_data->status = in_data->status;
  out_data->accX = in_data->accX;
  out_data->accZ = in_data->accZ;
  out_data->accY = in_data->accY;

  out_data->crc = calculate_cr8x_fast((uint8_t *)out_data + 1, 15);
}

/**
 * @brief USB HID Host interface callback
 */
void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle, const hid_host_interface_event_t event, void *arg)
{
  uint8_t data[64] = {0};
  size_t data_length = 0;
  hid_host_dev_params_t dev_params;
  ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

  switch (event)
  {
  case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
  {
    ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(hid_device_handle,
                                                              data,
                                                              64,
                                                              &data_length));

    usb_hid_data_get_callback(data);
    break;
  }
  case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
  {
    ESP_LOGI(USB_TAG, "HID Device, 'PS4' DISCONNECTED");
    ESP_ERROR_CHECK(hid_host_device_close(hid_device_handle));
    usb_hid_disconnect_callback();
    break;
  }
  case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
    ESP_LOGI(USB_TAG, "HID Device, 'PS4' TRANSFER_ERROR");
    esp_err_t err = hid_host_device_close(hid_device_handle);
    if (err != ESP_OK)
    {
      ESP_LOGW(USB_TAG, "Device close failed with error: %s", esp_err_to_name(err));
    }
    break;
  default:
    ESP_LOGE(USB_TAG, "HID Device, 'PS4' Unhandled event");
    break;
  }
}

/**
 * @brief USB HID Host Device event
 */
void hid_host_device_event(hid_host_device_handle_t hid_device_handle,
                           const hid_host_driver_event_t event,
                           void *arg)
{
  hid_host_dev_params_t dev_params;
  ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

  switch (event)
  {
  case HID_HOST_DRIVER_EVENT_CONNECTED:
    ESP_LOGI(USB_TAG, "HID Device, PS4 CONNECTED");

    const hid_host_device_config_t dev_config = {
        .callback = hid_host_interface_callback,
        .callback_arg = NULL};

    ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &dev_config));
    if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class)
    {
      ESP_ERROR_CHECK(hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT));
      if (HID_PROTOCOL_KEYBOARD == dev_params.proto)
      {
        ESP_ERROR_CHECK(hid_class_request_set_idle(hid_device_handle, 0, 0));
      }
    }
    ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));
    break;
  default:
    break;
  }
}

/**
 * @brief Start USB Host install
 */
static void usb_lib_task(void *arg)
{
  const usb_host_config_t host_config = {
      .skip_phy_setup = false,
      .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };

  ESP_ERROR_CHECK(usb_host_install(&host_config));
  xTaskNotifyGive(arg);

  while (true)
  {
    uint32_t event_flags;
    usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
    {
      ESP_ERROR_CHECK(usb_host_device_free_all());
      break;
    }
  }

  ESP_LOGI(USB_TAG, "USB shutdown");
  vTaskDelay(10);
  ESP_ERROR_CHECK(usb_host_uninstall());
  vTaskDelete(NULL);
}

/**
 * @brief HID Host Device callback
 */
void hid_host_device_callback(hid_host_device_handle_t hid_device_handle,
                              const hid_host_driver_event_t event,
                              void *arg)
{
  const app_event_queue_t evt_queue = {
      .event_group = APP_EVENT_HID_HOST,
      .hid_host_device.handle = hid_device_handle,
      .hid_host_device.event = event,
      .hid_host_device.arg = arg};

  if (app_event_queue)
  {
    xQueueSend(app_event_queue, &evt_queue, 0);
  }
}

#endif //__USB_HID_H