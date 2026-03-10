#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include "usb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "USB_CDC";

static bool usb_connected = false;
static bool usb_initialized = false;

// 수신 데이터 버퍼
#define USB_RX_BUFFER_SIZE 512
static char usb_rx_buffer[USB_RX_BUFFER_SIZE];
static size_t usb_rx_buffer_len = 0;

// 수신 데이터 콜백 함수 포인터
typedef void (*usb_rx_callback_t)(const char* data, size_t len);
static usb_rx_callback_t usb_rx_callback = NULL;

// USB Serial/JTAG 수신 데이터 처리 (메인 루프에서 주기적으로 호출)
// USB Serial/JTAG 드라이버를 통해 데이터를 읽습니다
static void usb_cdc_read_task(void)
{
    if (!usb_initialized) {
        return;
    }

    // USB Serial/JTAG 드라이버를 통해 데이터 읽기 (논블로킹)
    char temp_buffer[256];
    int bytes_read = usb_serial_jtag_read_bytes(temp_buffer, sizeof(temp_buffer) - 1, 0); // 0 = 논블로킹
    
    if (bytes_read > 0) {
        temp_buffer[bytes_read] = '\0';
        
        // 버퍼에 추가할 수 있는 공간 확인
        size_t available_space = sizeof(usb_rx_buffer) - 1 - usb_rx_buffer_len;
        size_t copy_len = (bytes_read < available_space) ? bytes_read : available_space;
        
        if (copy_len > 0) {
            memcpy(usb_rx_buffer + usb_rx_buffer_len, temp_buffer, copy_len);
            usb_rx_buffer_len += copy_len;
            usb_rx_buffer[usb_rx_buffer_len] = '\0';
            
            // 줄바꿈 문자를 찾아서 완전한 메시지 처리
            // 줄바꿈(\n) 또는 캐리지 리턴+줄바꿈(\r\n) 모두 처리
            char* newline_pos = strchr(usb_rx_buffer, '\n');
            if (newline_pos == NULL) {
                // \n이 없으면 \r도 확인
                newline_pos = strchr(usb_rx_buffer, '\r');
            }
            
            if (newline_pos != NULL) {
                // 줄바꿈까지의 길이 계산
                size_t message_len = newline_pos - usb_rx_buffer;
                
                // 콜백 함수가 등록되어 있으면 호출하고 버퍼에서 제거
                // 콜백이 없으면 버퍼에 남겨두어 usb_get_received_data()에서 가져갈 수 있도록 함
                if (usb_rx_callback != NULL) {
                    // 메시지 복사 (널 종료 문자 포함)
                    char message[USB_RX_BUFFER_SIZE];
                    memcpy(message, usb_rx_buffer, message_len);
                    message[message_len] = '\0';
                    
                    // 콜백 함수 호출
                    usb_rx_callback(message, message_len);
                    
                    // 버퍼에서 처리된 데이터 제거
                    size_t remaining = usb_rx_buffer_len - message_len - 1;
                    if (remaining > 0) {
                        memmove(usb_rx_buffer, newline_pos + 1, remaining);
                        usb_rx_buffer_len = remaining;
                        usb_rx_buffer[usb_rx_buffer_len] = '\0';
                    } else {
                        usb_rx_buffer_len = 0;
                        usb_rx_buffer[0] = '\0';
                    }
                }
                // 콜백이 없으면 버퍼에 데이터를 남겨둠 (usb_get_received_data()에서 가져감)
            }
        }
    }
}

esp_err_t usb_init(void)
{
    if (usb_initialized) {
        ESP_LOGW(TAG, "USB CDC가 이미 초기화되어 있습니다");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "USB Serial/JTAG 드라이버 초기화 시작");

    // USB Serial/JTAG 드라이버 설치 (데이터 전송 전용, 로그 출력과 분리)
    usb_serial_jtag_driver_config_t usb_serial_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t ret = usb_serial_jtag_driver_install(&usb_serial_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB Serial/JTAG 드라이버 설치 실패: %s", esp_err_to_name(ret));
        return ret;
    }
    
    usb_initialized = true;
    usb_connected = true;
    ESP_LOGI(TAG, "USB Serial/JTAG 드라이버 초기화 완료 (데이터 전송 전용, 로그는 UART0로 출력)");
    
    return ESP_OK;
}

esp_err_t usb_send_data(const char *data)
{
    if (!usb_initialized) {
        ESP_LOGE(TAG, "USB CDC가 초기화되지 않았습니다");
        return ESP_ERR_INVALID_STATE;
    }

    if (data == NULL) {
        ESP_LOGE(TAG, "전송할 데이터가 NULL입니다");
        return ESP_ERR_INVALID_ARG;
    }

    // USB Serial/JTAG 드라이버를 직접 사용하여 전송 (로그와 완전히 분리)
    size_t len = strlen(data);
    int written = usb_serial_jtag_write_bytes((const uint8_t*)data, len, portMAX_DELAY);
    
    if (written < 0 || (size_t)written != len) {
        ESP_LOGE(TAG, "USB 데이터 전송 실패: %d bytes written, expected %zu", written, len);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t usb_send_data_fmt(const char *format, ...)
{
    if (!usb_initialized) {
        ESP_LOGE(TAG, "USB CDC가 초기화되지 않았습니다");
        return ESP_ERR_INVALID_STATE;
    }

    if (format == NULL) {
        ESP_LOGE(TAG, "포맷 문자열이 NULL입니다");
        return ESP_ERR_INVALID_ARG;
    }

    // 가변 인자로 포맷 문자열 처리
    char buffer[512];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (len < 0 || len >= sizeof(buffer)) {
        ESP_LOGE(TAG, "포맷 문자열 처리 실패 또는 버퍼 오버플로우");
        return ESP_FAIL;
    }

    // 포맷된 문자열 전송
    return usb_send_data(buffer);
}

bool usb_is_connected(void)
{
    if (!usb_initialized) {
        return false;
    }
    
    // USB Serial/JTAG는 항상 연결된 것으로 간주
    // 실제 연결 상태는 드라이버에서 관리됨
    return usb_connected;
}

// 메인 루프에서 주기적으로 호출하여 수신 데이터 처리
void usb_cdc_task(void)
{
    if (usb_initialized) {
        usb_cdc_read_task();
    }
}

// 수신 데이터 콜백 함수 등록
void usb_set_rx_callback(usb_rx_callback_t callback)
{
    usb_rx_callback = callback;
}

// USB 통신 수신 준비 상태 관리
static bool usb_ready_done = false;
static bool usb_ready_callback_registered = false;

esp_err_t usb_ready_received(usb_rx_callback_t callback)
{
    // USB 초기화 (한 번만 실행)
    if (!usb_initialized) {
        ESP_LOGI(TAG, "USB 초기화 시작");
        esp_err_t ret = usb_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "USB CDC 초기화 실패: %s", esp_err_to_name(ret));
            return ret;
        }
        usb_ready_done = true;
        ESP_LOGI(TAG, "USB CDC 초기화 완료");
    }
    
    // USB 수신 콜백 함수 등록 (한 번만 실행)
    if (usb_initialized && callback != NULL && !usb_ready_callback_registered) {
        usb_set_rx_callback(callback);
        usb_ready_callback_registered = true;
        ESP_LOGI(TAG, "USB 수신 콜백 등록 완료");
    }
    
    // usb_cdc_task()는 외부에서 직접 호출하도록 함 (중복 호출 방지)
    
    return ESP_OK;
}

// 수신된 데이터 가져오기 (콜백 대신 직접 확인하는 경우)
bool usb_get_received_data(char* buffer, size_t buffer_size, size_t* data_len)
{
    if (!usb_initialized || buffer == NULL || buffer_size == 0) {
        return false;
    }
    
    // 버퍼가 비어있으면 false 반환
    if (usb_rx_buffer_len == 0) {
        return false;
    }
    
    // 줄바꿈이 있는 완전한 메시지만 반환
    // 줄바꿈(\n) 또는 캐리지 리턴(\r) 모두 처리
    char* newline_pos = strchr(usb_rx_buffer, '\n');
    if (newline_pos == NULL) {
        // \n이 없으면 \r도 확인
        newline_pos = strchr(usb_rx_buffer, '\r');
        if (newline_pos == NULL) {
            return false; // 완전한 메시지가 아직 없음
        }
    }
    
    size_t message_len = newline_pos - usb_rx_buffer;
    if (message_len >= buffer_size) {
        message_len = buffer_size - 1;
    }
    
    memcpy(buffer, usb_rx_buffer, message_len);
    buffer[message_len] = '\0';
    
    if (data_len != NULL) {
        *data_len = message_len;
    }
    
    // 버퍼에서 처리된 데이터 제거
    size_t remaining = usb_rx_buffer_len - message_len - 1;
    if (remaining > 0) {
        memmove(usb_rx_buffer, newline_pos + 1, remaining);
        usb_rx_buffer_len = remaining;
        usb_rx_buffer[usb_rx_buffer_len] = '\0';
    } else {
        usb_rx_buffer_len = 0;
        usb_rx_buffer[0] = '\0';
    }
    
    return true;
}

// 버퍼 길이 확인 (LED 상태 표시용)
size_t usb_get_buffer_len(void)
{
    return usb_rx_buffer_len;
}
