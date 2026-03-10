#ifndef GPIO_CONTROL_H
#define GPIO_CONTROL_H

#include "esp_err.h"
#include "driver/gpio.h"

// 핀 정의
#define BOOT_SWITCH_PIN     GPIO_NUM_0

// 작은 LED 3개 핀 정의
#define LED1_PIN            GPIO_NUM_16
#define LED2_PIN            GPIO_NUM_17  
#define LED3_PIN            GPIO_NUM_18

// WS2812 RGB LED 핀 정의
#define WS2812_GPIO_NUM     GPIO_NUM_48
#define WS2812_LED_NUMBERS  1

// 함수 선언
esp_err_t gpio_control_init(void);
void led_on(void);
void led_off(void);
int boot_switch_read(void);
bool boot_switch_is_pressed(void);

// WS2812 RGB LED 제어 함수
esp_err_t ws2812_init(void);
esp_err_t ws2812_set_color(uint8_t red, uint8_t green, uint8_t blue);
esp_err_t ws2812_set_brightness(uint8_t brightness);
esp_err_t ws2812_set_color_with_brightness(uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness);
esp_err_t ws2812_set_pixel(uint32_t index, uint8_t red, uint8_t green, uint8_t blue);
esp_err_t ws2812_refresh(void);
esp_err_t ws2812_clear(void);
esp_err_t ws2812_set_rgb(uint32_t rgb);
uint8_t ws2812_get_current_brightness(void);

// 간단 깜빡임 (태스크 기반, 기본 주기 사용)
esp_err_t ws2812_blink_start_rgb(uint8_t red, uint8_t green, uint8_t blue);
esp_err_t ws2812_blink_stop(void);

#endif // GPIO_CONTROL_H