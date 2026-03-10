#include "gpio_control.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "driver/rmt_tx.h"

static const char *TAG = "GPIO_CONTROL";

// WS2812 LED 스트립 핸들
static led_strip_handle_t led_strip = NULL;
static rmt_channel_handle_t led_chan = NULL;
static uint8_t current_brightness = 255;

// GPIO 제어 초기화
esp_err_t gpio_control_init(void)
{
    ESP_LOGI(TAG, "GPIO 제어 초기화 시작");
    
    // 부트 스위치 GPIO 설정
    gpio_config_t boot_switch_config = {
        .pin_bit_mask = (1ULL << BOOT_SWITCH_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    esp_err_t ret = gpio_config(&boot_switch_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "부트 스위치 GPIO 설정 실패: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "부트 스위치 GPIO 설정 완료 (GPIO%d)", BOOT_SWITCH_PIN);
    
    // 작은 LED 3개 GPIO 설정
    gpio_config_t led_config = {
        .pin_bit_mask = (1ULL << LED1_PIN) | (1ULL << LED2_PIN) | (1ULL << LED3_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    ret = gpio_config(&led_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED GPIO 설정 실패: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "LED GPIO 설정 완료 (LED1:GPIO%d, LED2:GPIO%d, LED3:GPIO%d)", 
             LED1_PIN, LED2_PIN, LED3_PIN);
    
    // WS2812 RGB LED 초기화 (선택적)
    esp_err_t ws2812_ret = ws2812_init();
    if (ws2812_ret != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 초기화 실패 (선택적 기능): %s", esp_err_to_name(ws2812_ret));
    }
    
    ESP_LOGI(TAG, "GPIO 제어 초기화 완료");
    
    return ESP_OK;
}

// LED 켜기 (모든 LED)
void led_on(void)
{
    gpio_set_level(LED1_PIN, 1);
    gpio_set_level(LED2_PIN, 1);
    gpio_set_level(LED3_PIN, 1);
    ESP_LOGI(TAG, "모든 LED 켜기 (LED1:GPIO%d, LED2:GPIO%d, LED3:GPIO%d)", 
             LED1_PIN, LED2_PIN, LED3_PIN);
}

// LED 끄기 (모든 LED)
void led_off(void)
{
    gpio_set_level(LED1_PIN, 0);
    gpio_set_level(LED2_PIN, 0);
    gpio_set_level(LED3_PIN, 0);
    ESP_LOGI(TAG, "모든 LED 끄기 (LED1:GPIO%d, LED2:GPIO%d, LED3:GPIO%d)", 
             LED1_PIN, LED2_PIN, LED3_PIN);
}

// 부트 스위치 상태 읽기
int boot_switch_read(void)
{
    return gpio_get_level(BOOT_SWITCH_PIN);
}

// 부트 스위치가 눌렸는지 확인
bool boot_switch_is_pressed(void)
{
    return (gpio_get_level(BOOT_SWITCH_PIN) == 0);
}

// WS2812 RGB LED 초기화
esp_err_t ws2812_init(void)
{
    if (led_strip != NULL) {
        return ESP_OK;
    }

    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = WS2812_GPIO_NUM,
        .mem_block_symbols = 64,
        .resolution_hz = 10 * 1000 * 1000,
        .trans_queue_depth = 4,
    };

    esp_err_t ret = rmt_new_tx_channel(&tx_chan_config, &led_chan);
    if (ret != ESP_OK) return ret;

    led_strip_config_t strip_config = {
        .strip_gpio_num = WS2812_GPIO_NUM,
        .max_leds = WS2812_LED_NUMBERS,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags = { .with_dma = false },
    };
    ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    if (ret != ESP_OK) return ret;

    ret = rmt_enable(led_chan);
    if (ret != ESP_OK) return ret;

    ws2812_clear();
    return ESP_OK;
}

// RGB 색상 설정 (밝기 적용)
esp_err_t ws2812_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
    return ws2812_set_color_with_brightness(red, green, blue, current_brightness);
}

// 밝기 설정 (0-255)
esp_err_t ws2812_set_brightness(uint8_t brightness)
{
    if (led_strip == NULL) return ESP_ERR_INVALID_STATE;
    current_brightness = brightness;
    return ESP_OK;
}

// 색상과 밝기를 함께 설정
esp_err_t ws2812_set_color_with_brightness(uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness)
{
    if (led_strip == NULL) return ESP_ERR_INVALID_STATE;
    uint8_t scaled_red = (red * brightness) / 255;
    uint8_t scaled_green = (green * brightness) / 255;
    uint8_t scaled_blue = (blue * brightness) / 255;
    current_brightness = brightness;
    for (int i = 0; i < WS2812_LED_NUMBERS; i++) {
        led_strip_set_pixel(led_strip, i, scaled_red, scaled_green, scaled_blue);
    }
    return led_strip_refresh(led_strip);
}

// 개별 LED 색상 설정
esp_err_t ws2812_set_pixel(uint32_t index, uint8_t red, uint8_t green, uint8_t blue)
{
    if (led_strip == NULL) return ESP_ERR_INVALID_STATE;
    if (index >= WS2812_LED_NUMBERS) return ESP_ERR_INVALID_ARG;
    uint8_t scaled_red = (red * current_brightness) / 255;
    uint8_t scaled_green = (green * current_brightness) / 255;
    uint8_t scaled_blue = (blue * current_brightness) / 255;
    led_strip_set_pixel(led_strip, index, scaled_red, scaled_green, scaled_blue);
    return ESP_OK;
}

// 변경사항 적용
esp_err_t ws2812_refresh(void)
{
    if (led_strip == NULL) return ESP_ERR_INVALID_STATE;
    return led_strip_refresh(led_strip);
}

// 모든 LED 끄기
esp_err_t ws2812_clear(void)
{
    if (led_strip == NULL) return ESP_ERR_INVALID_STATE;
    return led_strip_clear(led_strip);
}

// 0xRRGGBB 형식
esp_err_t ws2812_set_rgb(uint32_t rgb)
{
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;
    return ws2812_set_color(r, g, b);
}

// 현재 밝기 반환
uint8_t ws2812_get_current_brightness(void)
{
    return current_brightness;
}

// ======================================
// 간단 깜빡임 (태스크 기반, 기본 주기)
// ======================================
typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} ws2812_blink_rgb_t;

static TaskHandle_t s_ws2812_blink_task = NULL;
static volatile bool s_ws2812_blink_running = false;
static ws2812_blink_rgb_t s_ws2812_blink_color = {0, 0, 0};

static void ws2812_blink_task(void *pv)
{
    const TickType_t on_ticks = pdMS_TO_TICKS(300);
    const TickType_t off_ticks = pdMS_TO_TICKS(300);
    while (s_ws2812_blink_running) {
        ws2812_set_color(s_ws2812_blink_color.red, s_ws2812_blink_color.green, s_ws2812_blink_color.blue);
        vTaskDelay(on_ticks);
        if (!s_ws2812_blink_running) break;
        ws2812_set_color(0, 0, 0);
        vTaskDelay(off_ticks);
    }
    s_ws2812_blink_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t ws2812_blink_start_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (led_strip == NULL) return ESP_ERR_INVALID_STATE;

    // 이미 실행 중이면 색상만 갱신
    if (s_ws2812_blink_running && s_ws2812_blink_task != NULL) {
        s_ws2812_blink_color.red = red;
        s_ws2812_blink_color.green = green;
        s_ws2812_blink_color.blue = blue;
        return ESP_OK;
    }

    // 기존 태스크 정리 시도
    if (s_ws2812_blink_task != NULL) {
        s_ws2812_blink_running = false;
        for (int i = 0; i < 20 && s_ws2812_blink_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    s_ws2812_blink_color.red = red;
    s_ws2812_blink_color.green = green;
    s_ws2812_blink_color.blue = blue;
    s_ws2812_blink_running = true;
    if (xTaskCreate(ws2812_blink_task, "ws2812_blink", 3072, NULL, 3, &s_ws2812_blink_task) != pdPASS) {
        s_ws2812_blink_running = false;
        s_ws2812_blink_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ws2812_blink_stop(void)
{
    s_ws2812_blink_running = false;
    // 잠시 대기하여 태스크 종료 유도
    for (int i = 0; i < 20 && s_ws2812_blink_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // 확실히 끄기
    if (led_strip != NULL) {
        ws2812_set_color(0, 0, 0);
    }
    return ESP_OK;
}