#include "hub_led.h"
#include "gpio_control.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "HUB_LED";

static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;
static volatile hub_led_mode_t s_mode = HUB_LED_MODE_BOOT_NO_WIFI;
static volatile hub_led_err_t s_err = HUB_LED_ERR_NONE;
static bool s_inited;

static void hub_led_apply_solid(uint8_t r, uint8_t g, uint8_t b)
{
    ws2812_set_color(r, g, b);
}

static void hub_led_pulse_color(uint8_t r, uint8_t g, uint8_t b, int count)
{
    for (int i = 0; i < count; i++) {
        hub_led_apply_solid(r, g, b);
        vTaskDelay(pdMS_TO_TICKS(200));
        ws2812_clear();
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void hub_led_run_error_once(hub_led_err_t e)
{
    hub_led_pulse_color(255, 0, 0, 3);
    vTaskDelay(pdMS_TO_TICKS(400));

    switch (e) {
    case HUB_LED_ERR_HTTP:
        hub_led_pulse_color(255, 255, 0, 3);
        break;
    case HUB_LED_ERR_MQTT_INIT:
    case HUB_LED_ERR_MQTT_TRANSPORT:
        hub_led_pulse_color(255, 0, 255, 3);
        break;
    case HUB_LED_ERR_MQTT_CONNECT:
        hub_led_pulse_color(255, 0, 255, 2);
        break;
    case HUB_LED_ERR_MQTT_PUBLISH:
        hub_led_pulse_color(255, 0, 255, 1);
        break;
    case HUB_LED_ERR_WIFI:
        hub_led_pulse_color(255, 100, 0, 3);
        break;
    case HUB_LED_ERR_BLE_SCAN:
        hub_led_pulse_color(0, 0, 255, 3);
        break;
    case HUB_LED_ERR_BLE_CONNECT:
        hub_led_pulse_color(0, 0, 255, 2);
        break;
    case HUB_LED_ERR_BLE_SEND:
        hub_led_pulse_color(0, 0, 255, 1);
        break;
    case HUB_LED_ERR_NVS_SAVE:
        hub_led_pulse_color(0, 255, 255, 3);
        break;
    case HUB_LED_ERR_USB_CDC:
        hub_led_pulse_color(255, 0, 255, 3);
        break;
    case HUB_LED_ERR_MEMORY:
        hub_led_pulse_color(255, 0, 0, 5);
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
    default:
        break;
    }
    vTaskDelay(pdMS_TO_TICKS(600));
}

static void hub_led_render_normal(hub_led_mode_t m)
{
    switch (m) {
    case HUB_LED_MODE_BOOT_NO_WIFI:
        hub_led_apply_solid(255, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        break;

    case HUB_LED_MODE_USB_WAIT:
        /* 청록 1초 주기: 0.5s ON / 0.5s OFF */
        hub_led_apply_solid(0, 255, 255);
        vTaskDelay(pdMS_TO_TICKS(500));
        ws2812_clear();
        vTaskDelay(pdMS_TO_TICKS(500));
        break;

    case HUB_LED_MODE_USB_LINE_OK:
        hub_led_apply_solid(0, 255, 255);
        vTaskDelay(pdMS_TO_TICKS(200));
        break;

    case HUB_LED_MODE_WIFI_CONNECTING:
        hub_led_apply_solid(255, 100, 0);
        vTaskDelay(pdMS_TO_TICKS(400));
        ws2812_clear();
        vTaskDelay(pdMS_TO_TICKS(400));
        break;

    case HUB_LED_MODE_HTTP_CHECKING:
        hub_led_apply_solid(255, 255, 0);
        vTaskDelay(pdMS_TO_TICKS(400));
        ws2812_clear();
        vTaskDelay(pdMS_TO_TICKS(400));
        break;

    case HUB_LED_MODE_MQTT_CONNECTING:
        /* 초록 고정과 구분: 느린 깜빡임 */
        hub_led_apply_solid(0, 255, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
        ws2812_clear();
        vTaskDelay(pdMS_TO_TICKS(500));
        break;

    case HUB_LED_MODE_ONLINE:
        hub_led_apply_solid(0, 255, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        break;

    case HUB_LED_MODE_BLE_SCAN:
        hub_led_apply_solid(0, 0, 255);
        vTaskDelay(pdMS_TO_TICKS(400));
        ws2812_clear();
        vTaskDelay(pdMS_TO_TICKS(400));
        break;

    case HUB_LED_MODE_BLE_CONNECTED:
        hub_led_apply_solid(0, 0, 255);
        vTaskDelay(pdMS_TO_TICKS(200));
        break;

    default:
        vTaskDelay(pdMS_TO_TICKS(100));
        break;
    }
}

static void hub_led_task(void *arg)
{
    (void)arg;
    for (;;) {
        hub_led_err_t err;
        hub_led_mode_t mode;

        if (s_lock) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
        }
        err = s_err;
        mode = s_mode;
        if (s_lock) {
            xSemaphoreGive(s_lock);
        }

        if (err != HUB_LED_ERR_NONE) {
            hub_led_run_error_once(err);
            continue;
        }

        hub_led_render_normal(mode);
    }
}

void hub_led_init(uint8_t brightness)
{
    if (s_inited) {
        return;
    }
    ws2812_set_brightness(brightness);
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "mutex 생성 실패");
        return;
    }
    s_mode = HUB_LED_MODE_BOOT_NO_WIFI;
    s_err = HUB_LED_ERR_NONE;

    if (xTaskCreate(hub_led_task, "hub_led", 4096, NULL, 2, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "hub_led 태스크 생성 실패");
        return;
    }
    s_inited = true;
    ESP_LOGI(TAG, "허브 LED 표시 초기화 완료 (밝기=%u)", (unsigned)brightness);
}

void hub_led_set_mode(hub_led_mode_t mode)
{
    ws2812_blink_stop();

    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_mode = mode;
    s_err = HUB_LED_ERR_NONE;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

void hub_led_set_mode_if_changed(hub_led_mode_t mode)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_mode == mode && s_err == HUB_LED_ERR_NONE) {
        if (s_lock) {
            xSemaphoreGive(s_lock);
        }
        return;
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    hub_led_set_mode(mode);
}

void hub_led_set_error(hub_led_err_t err)
{
    if (err == HUB_LED_ERR_NONE) {
        hub_led_clear_error();
        return;
    }
    ws2812_blink_stop();
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_err = err;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

void hub_led_clear_error(void)
{
    ws2812_blink_stop();
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_err = HUB_LED_ERR_NONE;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}
