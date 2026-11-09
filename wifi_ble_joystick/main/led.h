#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "led_strip.h"
#include "sdkconfig.h"

#define BLINK_GPIO 48

static uint8_t s_led_state = 0;

static led_strip_handle_t led_strip;

static void configure_led(void)
{
  led_strip_config_t strip_config = {
      .strip_gpio_num = BLINK_GPIO,
      .max_leds = 1,
  };

  led_strip_rmt_config_t rmt_config = {
      .resolution_hz = 10 * 1000 * 1000,
      .flags.with_dma = false,
  };
  ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
  led_strip_clear(led_strip);
}

static void blink_led(void)
{
  if (s_led_state)
  {
    led_strip_set_pixel(led_strip, 0, 055, 055, 055);
    led_strip_refresh(led_strip);
  }
  else
  {
    led_strip_clear(led_strip);
  }
  s_led_state = !s_led_state;
}
