#ifndef __USB_UART_H
#define __USB_UART_H

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

#ifdef USB
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#endif

#include "easy_crc.h"

#define USB_TAG "USB"
#define UART_TAG "UART"
#define START_BYTE 0xA5

#define BUF_SIZE (1024)

static QueueHandle_t usb_uart_queue;

#ifdef USB
typedef struct
{
  uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1]; // Data buffer
  size_t buf_len;                                 // Number of bytes received
  uint8_t itf;                                    // Index of CDC device interface
} app_message_t;
#endif

typedef struct __attribute__((packed))
{
  uint8_t battery : 4; // Battery level (0-15)
  uint8_t usb : 1;     // USB connection status (1 if connected)
  uint8_t audio : 1;   // Audio jack connection status (1 if connected)
  uint8_t mic : 1;     // Microphone connection status (1 if connected)
  uint8_t unknown : 1; // Possibly for the extension port
} Status;

/**
 * @brief PS4 data type
 *
 * This datatype is used in the DS4 controller
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
    } __attribute__((packed));
    uint32_t val : 24;
  } __attribute__((packed)) buttons;
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

#pragma pack(push, 1)
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

#pragma pack(pop)

#endif //__USB_UART_H