#include "http_client.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "string.h"
#include "stdlib.h"
#include "cJSON.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"

static const char *TAG = "HTTP_CLIENT";

// HTTP 서버 URL 설정
// static const char* HTTP_SERVER_URL = "http://192.168.0.42:3080";
// static const char* HTTP_SERVER_URL = "http://192.168.0.20:3060";
// static const char* HTTP_SERVER_URL = "http://192.168.0.6:3080";
// static const char* HTTP_SERVER_URL = "http://192.168.0.7:5000/api";
// static const char* HTTP_SERVER_URL = "http://192.168.0.21:5000/api";
static const char* HTTP_SERVER_URL = "https://creamoff.o-r.kr/api";
// static const char* HTTP_SERVER_URL = "https://103.218.159.131/api";
// static const char* HTTP_SERVER_URL = "http://44.200.80.221:5000";

// HTTP 응답 데이터를 저장할 전역 변수
static char http_response_data[1024] = {0};
static int http_response_length = 0;

// HTTP 이벤트 핸들러
esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP_EVENT_ERROR 발생");
            if (evt->data) {
                ESP_LOGE(TAG, "에러 데이터: %.*s", evt->data_len, (char*)evt->data);
            }
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED - 서버 연결 성공");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // 응답 데이터 저장
                int copy_len = (evt->data_len < sizeof(http_response_data) - http_response_length) ? 
                              evt->data_len : (sizeof(http_response_data) - http_response_length - 1);
                if (copy_len > 0) {
                    memcpy(http_response_data + http_response_length, evt->data, copy_len);
                    http_response_length += copy_len;
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        default:
            break;
    }
    return ESP_OK;
}


// HTTP POST 요청 함수
char* send_http_post(const char* path, const char* data)
{
    if (path == NULL || data == NULL) {
        ESP_LOGE(TAG, "HTTP POST: path 또는 data가 NULL입니다");
        return NULL;
    }
    
    ESP_LOGI(TAG, "=== HTTP POST 요청 시작 ===");
    ESP_LOGI(TAG, "HTTP_SERVER_URL: %s", HTTP_SERVER_URL);
    ESP_LOGI(TAG, "Path: %s", path);
    ESP_LOGI(TAG, "Full URL: %s%s", HTTP_SERVER_URL, path);
    ESP_LOGI(TAG, "Data: %s", data);
    
    // WiFi 연결 상태 확인
    wifi_ap_record_t ap_info;
    esp_err_t wifi_ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (wifi_ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi가 연결되지 않음: %s", esp_err_to_name(wifi_ret));
        return NULL;
    }
    ESP_LOGI(TAG, "WiFi 연결됨 - SSID: %s, RSSI: %d", ap_info.ssid, ap_info.rssi);
    
    // IP 주소 할당 확인 및 대기
    esp_netif_t *netif = wifi_get_sta_netif();
    if (netif != NULL) {
        esp_netif_ip_info_t ip_info;
        esp_err_t ip_ret = esp_netif_get_ip_info(netif, &ip_info);
        if (ip_ret == ESP_OK && ip_info.ip.addr != 0) {
            char ip_str[16];
            inet_ntoa_r(ip_info.ip, ip_str, sizeof(ip_str));
            ESP_LOGI(TAG, "ESP32 IP 주소 확인: %s", ip_str);
        } else {
            ESP_LOGW(TAG, "IP 주소 할당 대기 중...");
            // IP 주소 할당 대기 (최대 5초)
            int wait_count = 0;
            while (wait_count < 50 && (ip_ret != ESP_OK || ip_info.ip.addr == 0)) {
                vTaskDelay(pdMS_TO_TICKS(100));
                ip_ret = esp_netif_get_ip_info(netif, &ip_info);
                wait_count++;
            }
            if (ip_ret == ESP_OK && ip_info.ip.addr != 0) {
                char ip_str[16];
                inet_ntoa_r(ip_info.ip, ip_str, sizeof(ip_str));
                ESP_LOGI(TAG, "IP 주소 할당 완료: %s", ip_str);
            } else {
                ESP_LOGE(TAG, "IP 주소 할당 실패 - HTTP 요청 중단");
                return NULL;
            }
        }
    }
    
    // 네트워크 스택 안정화 대기
    ESP_LOGI(TAG, "네트워크 스택 안정화 대기 중...");
    vTaskDelay(pdMS_TO_TICKS(2000));  // 2초 대기
    
    // 게이트웨이 연결 확인 (라우팅 테이블 초기화 확인)
    if (netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.gw.addr != 0) {
            char gw_str[16];
            inet_ntoa_r(ip_info.gw, gw_str, sizeof(gw_str));
            ESP_LOGI(TAG, "게이트웨이 확인: %s", gw_str);
        }
    }
    
    // 전체 URL 구성
    char full_url[256];
    snprintf(full_url, sizeof(full_url), "%s%s", HTTP_SERVER_URL, path);
    
    /* HTTPS TLS는 내부 RAM에 큰 버퍼를 씀 — 힙 부족 시 mbedtls_ssl_setup -0x7F00 (ALLOC_FAILED) */
    ESP_LOGI(TAG, "heap 내부RAM: %u, 최소: %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    // HTTP 클라이언트 설정 (HTTPS 지원)
    esp_http_client_config_t config = {
        .url = full_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,  // 30초 타임아웃
        .event_handler = http_event_handler,
        .transport_type = HTTP_TRANSPORT_UNKNOWN,  // HTTP/HTTPS 자동 감지
        .crt_bundle_attach = esp_crt_bundle_attach,  // ESP32 기본 CA 인증서 번들 사용
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    
    // HTTP 클라이언트 초기화
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "HTTP 클라이언트 초기화 실패");
        return NULL;
    }
    
    // Content-Type 헤더 설정
    esp_http_client_set_header(client, "Content-Type", "application/json");
    
    // 요청 본문: 호출자가 넘긴 JSON을 그대로 사용 (트렁케이션 방지)
    ESP_LOGI(TAG, "JSON Length: %d", (int)strlen(data));
    ESP_LOGD(TAG, "JSON Data: %s", data);
    
    // POST 데이터 설정
    esp_http_client_set_post_field(client, data, strlen(data));
    
    // 응답 데이터 초기화
    memset(http_response_data, 0, sizeof(http_response_data));
    http_response_length = 0;
    
    // HTTP 요청 실행
    esp_err_t err = esp_http_client_perform(client);
    char* response_data = NULL;
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);
        ESP_LOGI(TAG, "HTTP POST 성공 - Status: %d, Content-Length: %d", status_code, content_length);
        
        // 응답 데이터 처리
        if (http_response_length > 0) {
            http_response_data[http_response_length] = '\0';
            ESP_LOGI(TAG, "=== 서버 응답 ===");
            ESP_LOGI(TAG, "응답 데이터: %s", http_response_data);
            ESP_LOGI(TAG, "================");
            
            // 응답 데이터를 동적으로 할당하여 복사
            response_data = malloc(http_response_length + 1);
            if (response_data != NULL) {
                strcpy(response_data, http_response_data);
            }
        } else {
            ESP_LOGW(TAG, "응답 데이터가 없습니다");
        }
    } else {
        ESP_LOGE(TAG, "HTTP POST 실패: %s", esp_err_to_name(err));
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGE(TAG, "HTTP 상태 코드: %d", status_code);
        ESP_LOGE(TAG, "요청 URL: %s", full_url);
    }
    
    // HTTP 클라이언트 정리
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "=== HTTP POST 요청 완료 ===");
    
    return response_data;
}


// HTTP POST 요청 함수 (문자열 전송 전용 - text/plain)
char* send_http_post_string(const char* path, const char* data)
{
    if (path == NULL || data == NULL) {
        ESP_LOGE(TAG, "HTTP POST STRING: path 또는 data가 NULL입니다");
        return NULL;
    }
    
    ESP_LOGI(TAG, "=== HTTP POST STRING 요청 시작 ===");
    ESP_LOGI(TAG, "HTTP_SERVER_URL: %s", HTTP_SERVER_URL);
    ESP_LOGI(TAG, "Path: %s", path);
    ESP_LOGI(TAG, "Full URL: %s%s", HTTP_SERVER_URL, path);
    ESP_LOGI(TAG, "Plain Data: %s", data);
    
    // WiFi 연결 상태 확인
    wifi_ap_record_t ap_info;
    esp_err_t wifi_ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (wifi_ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi가 연결되지 않음: %s", esp_err_to_name(wifi_ret));
        return NULL;
    }
    
    // 전체 URL 구성
    char full_url[256];
    snprintf(full_url, sizeof(full_url), "%s%s", HTTP_SERVER_URL, path);
    
    // HTTP 클라이언트 설정
    esp_http_client_config_t config = {
        .url = full_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,  // 타임아웃 증가 (30초)
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .skip_cert_common_name_check = true,
        .event_handler = http_event_handler,
        .disable_auto_redirect = true,
        .max_redirection_count = 0,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,  // TCP 전송 명시
        .keep_alive_enable = false,  // Keep-Alive 비활성화
    };
    
    // HTTP 클라이언트 초기화
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "HTTP 클라이언트 초기화 실패");
        return NULL;
    }
    
    // HTTP 헤더 설정
    esp_http_client_set_header(client, "Content-Type", "text/plain");
    esp_http_client_set_header(client, "User-Agent", "ESP32-HTTP-Client/1.0");
    esp_http_client_set_header(client, "Connection", "close");
    esp_http_client_set_header(client, "Accept", "*/*");
    
    // POST 데이터 설정 (그대로 전송)
    esp_http_client_set_post_field(client, data, strlen(data));
    
    // 응답 버퍼 초기화
    memset(http_response_data, 0, sizeof(http_response_data));
    http_response_length = 0;
    
    // 연결 전 짧은 대기 (네트워크 안정화)
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // HTTP 요청 실행
    esp_err_t err = esp_http_client_perform(client);
    char* response_data = NULL;
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);
        ESP_LOGI(TAG, "HTTP POST STRING 성공 - Status: %d, Content-Length: %d", status_code, content_length);
        
        if (http_response_length > 0) {
            http_response_data[http_response_length] = '\0';
            response_data = malloc(http_response_length + 1);
            if (response_data != NULL) {
                strcpy(response_data, http_response_data);
            }
        } else {
            ESP_LOGW(TAG, "응답 데이터가 없습니다");
        }
    } else {
        ESP_LOGE(TAG, "HTTP POST STRING 실패: %s", esp_err_to_name(err));
    }
    
    // 정리
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "=== HTTP POST STRING 요청 완료 ===");
    
    return response_data;
}

// JSON에서 boolean 값 가져오기
bool get_json_bool(const char* json_str, const char* key) {
    if (json_str == NULL || key == NULL) {
        return false;
    }
    
    cJSON* json = cJSON_Parse(json_str);
    if (json == NULL) {
        ESP_LOGW(TAG, "JSON 파싱 실패: %s", cJSON_GetErrorPtr());
        return false;
    }
    
    cJSON* item = cJSON_GetObjectItemCaseSensitive(json, key);
    bool result = false;
    
    if (cJSON_IsBool(item)) {
        result = cJSON_IsTrue(item);
    }
    
    cJSON_Delete(json);
    return result;
}

// JSON에서 문자열 값 가져오기 (호출자가 free 해야 함)
char* get_json_string(const char* json_str, const char* key) {
    if (json_str == NULL || key == NULL) {
        return NULL;
    }
    
    cJSON* json = cJSON_Parse(json_str);
    if (json == NULL) {
        ESP_LOGW(TAG, "JSON 파싱 실패: %s", cJSON_GetErrorPtr());
        return NULL;
    }
    
    cJSON* item = cJSON_GetObjectItemCaseSensitive(json, key);
    char* result = NULL;
    
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        result = malloc(strlen(item->valuestring) + 1);
        if (result != NULL) {
            strcpy(result, item->valuestring);
        }
    }
    
    cJSON_Delete(json);
    return result;
}
