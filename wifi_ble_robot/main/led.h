#ifndef __LED_H
#define __LED_H

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"

#define BLINK_GPIO 33
#define WIFI_LED_GPIO 2

static uint8_t s_led_state = 0;

static void configure_led(void)
{
    gpio_config_t blink_led_config = {
        .pin_bit_mask = (1ULL << BLINK_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&blink_led_config));
    gpio_set_level(BLINK_GPIO, 0);  /* LED off initially */

    gpio_config_t wifi_led_config = {
        .pin_bit_mask = (1ULL << WIFI_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&wifi_led_config));
    gpio_set_level(WIFI_LED_GPIO, 0);  /* LED off initially */
}

static void blink_led(void)
{
  if (s_led_state) {
    gpio_set_level(BLINK_GPIO, 1);
  } else {
    gpio_set_level(BLINK_GPIO, 0);
  }
  s_led_state = !s_led_state;
}

static void led_show_ip_acquired(void)
{
  gpio_set_level(WIFI_LED_GPIO, 1); 
}

static void led_show_no_ip(void)
{
  gpio_set_level(WIFI_LED_GPIO, 0); 
}

#endif //__LED_H