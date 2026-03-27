#include "mqtt.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "hub_led.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "MQTT_CLIENT";

// MQTT 클라이언트 핸들
static esp_mqtt_client_handle_t mqtt_client = NULL;

// MQTT 초기화 상태
static bool mqtt_initialized = false;

// MQTT 연결 상태
static bool mqtt_connected = false;

// 전송 시간 측정을 위한 변수
static int last_sent_msg_id = -1;
static int64_t last_send_start_time = 0;

// 테스트 전송 카운터
static int mqtt_test_count = 0;

// MQTT 연결 대기 관련 변수
static int mqtt_connect_wait_count = 0;
static int mqtt_init_retry_count = 0;

// MQTT 브로커 설정 (필요에 따라 수정)
// #define MQTT_BROKER_URL "mqtt://192.168.0.7"
// #define MQTT_BROKER_URL "mqtt://192.168.0.21"
// #define MQTT_BROKER_URL "mqtt://localhost"
#define MQTT_BROKER_URL "mqtt://103.218.159.131"
// #define MQTT_BROKER_URL "mqtt://44.200.80.221"
#define MQTT_BROKER_PORT 1883
#define MQTT_USERNAME NULL  // 필요시 설정
#define MQTT_PASSWORD NULL  // 필요시 설정

// MQTT 동적 설정 (MAC 주소 기반으로 생성됨)
static char mqtt_client_id[64] = {0};  // 클라이언트 ID (esp32_AA:BB:CC:DD:EE 형식)
static char mqtt_send_topic[64] = {0};  // esp32에서 backend로 데이터 전송용 토픽
static char mqtt_subscribe_topic[64] = {0};  // Backend에서 ESP32로 명령 전송용 토픽

// MQTT 연결 대기 최대 시간 (초)
#define MQTT_CONNECT_WAIT_MAX_SEC 10
// MQTT 초기화 최대 재시도 횟수
#define MQTT_INIT_MAX_RETRIES 3
// MQTT 최대 메시지 크기 (bytes)
// MQTT 프로토콜은 큰 메시지를 지원하지만, ESP32 메모리 제약을 고려하여 설정
// 일반적으로 수 KB ~ 수십 KB까지는 문제없음
#define MQTT_MAX_MESSAGE_SIZE 8192  // 8KB로 증가 (필요시 더 늘릴 수 있음)

// MQTT 토픽 설정 함수 (MAC 주소 기반)
void mqtt_set_topics(const char* mac_address)
{
    if (mac_address == NULL || strlen(mac_address) == 0) {
        ESP_LOGE(TAG, "유효하지 않은 MAC 주소");
        return;
    }

    // 클라이언트 ID 생성: esp32_AA:BB:CC:DD:EE
    snprintf(mqtt_client_id, sizeof(mqtt_client_id), "esp32_%s", mac_address);

    // 토픽 생성
    snprintf(mqtt_send_topic, sizeof(mqtt_send_topic), "hub/%s/send", mac_address);
    snprintf(mqtt_subscribe_topic, sizeof(mqtt_subscribe_topic), "hub/%s/receive", mac_address);

    ESP_LOGI(TAG, "MQTT 설정 완료:");
    ESP_LOGI(TAG, "  - 클라이언트 ID: %s", mqtt_client_id);
    ESP_LOGI(TAG, "  - 전송 토픽: %s", mqtt_send_topic);
    ESP_LOGI(TAG, "  - 수신 토픽: %s", mqtt_subscribe_topic);
}

// MQTT 이벤트 핸들러
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        printf("[MQTT] 연결 성공 - 브로커: %s:%d\n", MQTT_BROKER_URL, MQTT_BROKER_PORT);
        fflush(stdout);
        ESP_LOGI(TAG, "MQTT 연결 성공 - 브로커: %s:%d", MQTT_BROKER_URL, MQTT_BROKER_PORT);
        mqtt_connected = true;
        
        // Backend에서 보내는 명령 토픽 구독
        ESP_LOGI(TAG, "토픽 구독 시도: %s", mqtt_subscribe_topic);
        msg_id = esp_mqtt_client_subscribe(client, mqtt_subscribe_topic, 1);
        if (msg_id >= 0) {
            ESP_LOGI(TAG, "토픽 구독 요청 완료, msg_id=%d", msg_id);
        } else {
            ESP_LOGE(TAG, "토픽 구독 요청 실패, msg_id=%d", msg_id);
        }
        break;
        
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT 연결 해제");
        mqtt_connected = false;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "토픽 구독 완료, msg_id=%d", event->msg_id);
        break;
        
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "토픽 구독 해제, msg_id=%d", event->msg_id);
        break;
        
    case MQTT_EVENT_PUBLISHED:
        {
            /* MQTT 발행 성공은 LED로 표시하지 않음(정상 유휴=초록 고정과 혼동 방지). */

            // printf("[MQTT] 메시지 발행 완료, msg_id=%d\n", event->msg_id);
            fflush(stdout);
            // ESP_LOGI(TAG, "메시지 발행 완료, msg_id=%d", event->msg_id);
            
            // 브로커 도달 시간 측정
            if (last_sent_msg_id == event->msg_id && last_send_start_time > 0) {
                int64_t current_time = esp_timer_get_time();
                int64_t elapsed_us = current_time - last_send_start_time;
                float elapsed_ms = elapsed_us / 1000.0f;
                
                // printf("[MQTT] 브로커 도달 시간: %.2f ms (%.3f 초)\n", elapsed_ms, elapsed_ms / 1000.0f);
                fflush(stdout);
                // ESP_LOGI(TAG, "브로커 도달 시간: %.2f ms (%.3f 초)", elapsed_ms, elapsed_ms / 1000.0f);
                
                // 측정 완료 후 초기화
                last_sent_msg_id = -1;
                last_send_start_time = 0;
            }
        }
        break;
        
    case MQTT_EVENT_DATA:
        {
            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, "📨 MQTT 데이터 수신!");
            ESP_LOGI(TAG, "========================================");
            
            // 토픽 정보 출력
            char topic[128] = {0};
            int topic_len = event->topic_len < sizeof(topic) - 1 ? event->topic_len : sizeof(topic) - 1;
            memcpy(topic, event->topic, topic_len);
            topic[topic_len] = '\0';
            ESP_LOGI(TAG, "토픽: %s", topic);
            
            // 데이터 정보 출력
            ESP_LOGI(TAG, "데이터 크기: %d bytes", event->data_len);
            
            // 데이터 내용 출력 (최대 500자)
            char data_str[501] = {0};
            int data_len = event->data_len < 500 ? event->data_len : 500;
            memcpy(data_str, event->data, data_len);
            data_str[data_len] = '\0';
            ESP_LOGI(TAG, "데이터 내용: %s", data_str);
            
            if (event->data_len > 500) {
                ESP_LOGI(TAG, "... (총 %d bytes, 처음 500자만 표시)", event->data_len);
            }
            
            ESP_LOGI(TAG, "========================================");
            
            // 수신한 데이터를 전역 변수에 저장 (main.c에서 접근 가능하도록)
            // extern 변수 선언 필요
            extern char mqtt_received_data[512];
            extern bool mqtt_data_received;
            
            // 전체 데이터 복사 (최대 511바이트)
            int copy_len = event->data_len < 511 ? event->data_len : 511;
            memcpy(mqtt_received_data, event->data, copy_len);
            mqtt_received_data[copy_len] = '\0';
            mqtt_data_received = true;
            
            // ESP_LOGI(TAG, "MQTT 데이터 저장 완료: %s", mqtt_received_data);
        }
        break;
        
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "========================================");
        ESP_LOGE(TAG, "MQTT 오류 발생!");
        ESP_LOGE(TAG, "========================================");
        ESP_LOGE(TAG, "오류 타입: %d", event->error_handle->error_type);
        
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "TCP 전송 오류: %s", strerror(event->error_handle->esp_transport_sock_errno));
            ESP_LOGE(TAG, "에러 코드: %d", event->error_handle->esp_transport_sock_errno);
            ESP_LOGE(TAG, "브로커 주소: %s:%d", MQTT_BROKER_URL, MQTT_BROKER_PORT);
            ESP_LOGE(TAG, "가능한 원인:");
            ESP_LOGE(TAG, "  1. 브로커가 실행 중이지 않음");
            ESP_LOGE(TAG, "  2. 브로커가 외부 연결을 허용하지 않음 (localhost만 허용)");
            ESP_LOGE(TAG, "  3. 방화벽이 포트를 차단함");
            ESP_LOGE(TAG, "  4. IP 주소가 잘못됨");
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "연결 거부됨");
            ESP_LOGE(TAG, "가능한 원인:");
            ESP_LOGE(TAG, "  1. 브로커가 인증을 요구함");
            ESP_LOGE(TAG, "  2. 클라이언트 ID가 이미 사용 중");
            ESP_LOGE(TAG, "  3. 브로커 설정에서 연결을 거부함");
        } else {
            ESP_LOGE(TAG, "알 수 없는 오류 타입");
        }
        ESP_LOGE(TAG, "========================================");
        mqtt_connected = false;
        hub_led_set_error(HUB_LED_ERR_MQTT_TRANSPORT);
        break;
        
    case MQTT_EVENT_BEFORE_CONNECT:
        ESP_LOGI(TAG, "MQTT 연결 시도 시작...");
        ESP_LOGI(TAG, "  브로커: %s:%d", MQTT_BROKER_URL, MQTT_BROKER_PORT);
        ESP_LOGI(TAG, "  클라이언트 ID: %s", mqtt_client_id);
        break;
        
    default:
        ESP_LOGI(TAG, "기타 MQTT 이벤트: %d", event->event_id);
        break;
    }
}

// MQTT 초기화 함수
esp_err_t mqtt_init(void)
{
    if (mqtt_initialized) {
        ESP_LOGW(TAG, "MQTT가 이미 초기화되어 있습니다");
        return ESP_OK;
    }

    // WiFi 연결 상태 확인
    if (!wifi_connected) {
        ESP_LOGE(TAG, "WiFi가 연결되지 않았습니다. WiFi 연결 후 MQTT를 초기화하세요.");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "WiFi 연결 확인됨 - MQTT 초기화 진행");

    ESP_LOGI(TAG, "MQTT 초기화 시작");

    // MQTT 브로커 URL 구성 - 포트를 포함한 전체 URI
    char mqtt_broker_url[128];
    // mqtt:// 형식에서 포트는 URI에 포함하거나 hostname과 port를 분리
    // ESP-IDF는 hostname과 port를 분리하는 방식을 선호
    // MQTT_BROKER_URL에서 호스트 추출 (mqtt:// 제거)
    const char* host_start = MQTT_BROKER_URL;
    if (strncmp(host_start, "mqtt://", 7) == 0) {
        host_start += 7;  // "mqtt://" 제거
    } else if (strncmp(host_start, "mqtts://", 8) == 0) {
        host_start += 8;  // "mqtts://" 제거
    }
    snprintf(mqtt_broker_url, sizeof(mqtt_broker_url), "mqtt://%s:%d", host_start, MQTT_BROKER_PORT);
    ESP_LOGI(TAG, "MQTT 브로커 URL: %s", mqtt_broker_url);

    // MQTT 클라이언트 설정
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = mqtt_broker_url,
        .credentials.client_id = mqtt_client_id,  // 동적으로 생성된 클라이언트 ID 사용
    };

    // 사용자 이름과 비밀번호가 설정되어 있다면 추가
    if (MQTT_USERNAME != NULL) {
        mqtt_cfg.credentials.username = MQTT_USERNAME;
    }
    if (MQTT_PASSWORD != NULL) {
        mqtt_cfg.credentials.authentication.password = MQTT_PASSWORD;
    }

    // MQTT 클라이언트 생성
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT 클라이언트 초기화 실패");
        return ESP_FAIL;
    }

    // 이벤트 핸들러 등록
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    // MQTT 클라이언트 시작
    ESP_LOGI(TAG, "MQTT 클라이언트 시작 중...");
    ESP_LOGI(TAG, "  연결 대상: %s", mqtt_broker_url);
    ESP_LOGI(TAG, "  클라이언트 ID: %s", mqtt_client_id);
    
    esp_err_t ret = esp_mqtt_client_start(mqtt_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MQTT 클라이언트 시작 실패: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "에러 코드: 0x%x", ret);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        return ret;
    }

    mqtt_initialized = true;
    ESP_LOGI(TAG, "MQTT 클라이언트 시작 완료 - 연결 대기 중...");

    return ESP_OK;
}

// MQTT 초기화 여부 확인
bool mqtt_is_initialized(void)
{
    return mqtt_initialized;
}

// MQTT 초기화 완료 확인 (별칭 - 사용 편의성)
bool mqtt_is_init(void)
{
    return mqtt_initialized && mqtt_connected;
}

// MQTT 연결 여부 확인
bool mqtt_is_connected(void)
{
    return mqtt_connected;
}

// MQTT 데이터 전송 함수
esp_err_t mqtt_send_data(const char* topic, const char* data)
{
    if (!mqtt_initialized || mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT가 초기화되지 않았습니다");
        return ESP_ERR_INVALID_STATE;
    }

    if (!mqtt_connected) {
        ESP_LOGW(TAG, "MQTT가 연결되지 않았습니다. 전송 대기 중...");
        return ESP_ERR_INVALID_STATE;
    }

    if (topic == NULL || data == NULL) {
        ESP_LOGE(TAG, "토픽 또는 데이터가 NULL입니다");
        return ESP_ERR_INVALID_ARG;
    }

    // 전송 시작 시간 측정
    int64_t start_time = esp_timer_get_time();
    
    // QoS 1로 설정 (최소 한 번 전달 보장), retain 0 (메시지 유지 안함)
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, data, strlen(data), 1, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "MQTT 메시지 발행 실패 - msg_id: %d", msg_id);
        return ESP_FAIL;
    }

    // 전송 시작 시간과 msg_id 저장 (브로커 도달 시간 측정용)
    last_sent_msg_id = msg_id;
    last_send_start_time = start_time;

    ESP_LOGI(TAG, "MQTT 메시지 발행 시작 - 토픽: %s, 데이터 길이: %zu, msg_id: %d", topic, strlen(data), msg_id);
    return ESP_OK;
}

// MQTT 데이터 수신 처리 함수 (주기적으로 호출하여 메시지 처리)
void mqtt_receive_data(void)
{
    // MQTT 이벤트는 비동기적으로 처리되므로
    // 이 함수는 주로 상태 확인이나 추가 처리를 위해 사용할 수 있습니다
    // 실제 데이터 수신은 mqtt_event_handler의 MQTT_EVENT_DATA에서 처리됩니다
    
    if (!mqtt_initialized) {
        return;
    }

    // 연결이 끊어진 경우 재연결 시도
    if (!mqtt_connected && mqtt_client != NULL) {
        ESP_LOGI(TAG, "MQTT 재연결 시도 중...");
        // esp_mqtt_client_reconnect()는 자동으로 재연결을 시도하지만,
        // 필요시 수동으로 재시작할 수 있습니다
    }
}

// MQTT 연결 해제
esp_err_t mqtt_deinit(void)
{
    if (!mqtt_initialized) {
        ESP_LOGW(TAG, "MQTT가 초기화되지 않았습니다");
        return ESP_OK;
    }

    if (mqtt_client != NULL) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
    }

    mqtt_initialized = false;
    mqtt_connected = false;
    mqtt_connect_wait_count = 0;
    mqtt_init_retry_count = 0;
    ESP_LOGI(TAG, "MQTT 해제 완료");

    return ESP_OK;
}

// 토픽 유효성 검증
static bool mqtt_validate_topic(const char* topic)
{
    if (topic == NULL) {
        return false;
    }
    
    size_t len = strlen(topic);
    if (len == 0 || len >= 64) {  // MQTT 토픽 최대 길이 제한
        ESP_LOGE(TAG, "토픽 길이 오류: %zu (최대 63자)", len);
        return false;
    }
    
    // 토픽에 NULL 문자나 잘못된 문자가 있는지 확인
    for (size_t i = 0; i < len; i++) {
        if (topic[i] == '\0' || topic[i] == '\n' || topic[i] == '\r') {
            ESP_LOGE(TAG, "토픽에 잘못된 문자가 포함됨");
            return false;
        }
    }
    
    return true;
}

// 데이터 크기 검증
static bool mqtt_validate_data_size(size_t data_size)
{
    if (data_size == 0) {
        ESP_LOGE(TAG, "데이터 크기가 0입니다");
        return false;
    }
    
    if (data_size > MQTT_MAX_MESSAGE_SIZE) {
        // ESP_LOGE(TAG, "데이터 크기 초과: %zu bytes (최대: %d bytes)", data_size, MQTT_MAX_MESSAGE_SIZE);
        return false;
    }
    
    return true;
}

// 테스트용 JSON 데이터 생성 (이전에 작성한 코드와 동일한 형태)
static char* mqtt_create_test_json(const char* ip_address)
{
    ESP_LOGI(TAG, "=== JSON 데이터 생성 시작 ===");
    
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        ESP_LOGE(TAG, "JSON 객체 생성 실패");
        return NULL;
    }
    
    // 테스트 카운터 증가
    mqtt_test_count++;
    ESP_LOGI(TAG, "테스트 카운터: %d", mqtt_test_count);
    
    // 기본 필드 추가 (이전에 작성한 코드와 동일)
    cJSON_AddStringToObject(json, "address", "AA:BB:CC:DD:EE:FF");
    cJSON_AddNumberToObject(json, "sampling_rate", 90);
    cJSON_AddNumberToObject(json, "spo2", 100);
    cJSON_AddNumberToObject(json, "hr", mqtt_test_count);  // 카운터 값 사용 (순서 확인용)
    cJSON_AddNumberToObject(json, "temp", 32.5);
    cJSON_AddNumberToObject(json, "bat", 100);
    
    // IP 주소 추가 (이전에 작성한 코드와 동일)
    if (ip_address != NULL && strlen(ip_address) > 0) {
        cJSON_AddStringToObject(json, "ip", ip_address);
        ESP_LOGI(TAG, "IP 주소 추가: %s", ip_address);
    } else {
        cJSON_AddStringToObject(json, "ip", "0.0.0.0");
        ESP_LOGI(TAG, "IP 주소 없음 - 기본값 사용: 0.0.0.0");
    }
    
    // raw 배열 생성 (50개 데이터) - 이전에 작성한 코드와 동일
    cJSON *raw_array = cJSON_CreateArray();
    if (raw_array == NULL) {
        ESP_LOGE(TAG, "raw 배열 생성 실패");
        cJSON_Delete(json);
        return NULL;
    }
    
    // raw 배열에 50개의 데이터 추가 (이전에 작성한 코드와 동일)
    ESP_LOGI(TAG, "raw 배열에 50개 데이터 추가 중...");
    for (int i = 0; i < 50; i++) {
        cJSON *item = cJSON_CreateString("123456,654321,123456");
        if (item == NULL) {
            ESP_LOGE(TAG, "raw 배열 항목 생성 실패");
            cJSON_Delete(raw_array);
            cJSON_Delete(json);
            return NULL;
        }
        cJSON_AddItemToArray(raw_array, item);
    }
    
    cJSON_AddItemToObject(json, "raw", raw_array);
    ESP_LOGI(TAG, "raw 배열 추가 완료 (50개 항목)");
    
    // JSON을 문자열로 변환 (이전에 작성한 코드와 동일)
    char *json_string = cJSON_Print(json);
    cJSON_Delete(json);  // JSON 객체는 삭제 (문자열은 별도 메모리)
    
    if (json_string == NULL) {
        ESP_LOGE(TAG, "JSON 문자열 변환 실패");
        return NULL;
    }
    
    ESP_LOGI(TAG, "=== JSON 데이터 생성 완료 ===");
    ESP_LOGI(TAG, "생성된 JSON (처음 200자): %.200s...", json_string);
    
    return json_string;
}

// MQTT 연결 준비 상태 확인 및 초기화 (WiFi 확인 포함)
bool mqtt_ensure_ready(void)
{
    // WiFi 연결 상태 확인
    if (!wifi_connected) {
        ESP_LOGW(TAG, "WiFi 미연결 - MQTT 초기화 불가");
        return false;
    }
    
    // MQTT 초기화 확인 및 초기화
    if (!mqtt_initialized) {
        ESP_LOGI(TAG, "MQTT 초기화 시작...");
        esp_err_t ret = mqtt_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "MQTT 초기화 실패: %s", esp_err_to_name(ret));
            mqtt_init_retry_count++;
            
            // 최대 재시도 횟수 초과 시
            if (mqtt_init_retry_count >= MQTT_INIT_MAX_RETRIES) {
                ESP_LOGE(TAG, "MQTT 초기화 최대 재시도 횟수 도달");
                mqtt_init_retry_count = 0;
                return false;
            }
            return false;
        }
        ESP_LOGI(TAG, "MQTT 초기화 완료");
        mqtt_init_retry_count = 0;  // 성공 시 리셋
    }
    
    // MQTT 연결 대기
    if (!mqtt_connected) {
        mqtt_connect_wait_count++;
        if (mqtt_connect_wait_count < MQTT_CONNECT_WAIT_MAX_SEC) {
            // 아직 대기 중
            return false;
        } else {
            // 최대 대기 시간 초과 - 재초기화 시도
            ESP_LOGW(TAG, "MQTT 연결 대기 시간 초과 - 재초기화 필요");
            mqtt_deinit();
            mqtt_connect_wait_count = 0;
            return false;
        }
    }
    
    // 연결 성공 시 카운터 리셋
    mqtt_connect_wait_count = 0;
    return true;
}

// MQTT 테스트 전송 함수 (모든 로직 통합)
esp_err_t mqtt_test_send(const char* ip_address)
{
    // WiFi 연결 상태 확인
    if (!wifi_connected) {
        ESP_LOGE(TAG, "WiFi 미연결 - MQTT 전송 불가");
        return ESP_ERR_INVALID_STATE;
    }
    
    // MQTT 준비 상태 확인
    if (!mqtt_ensure_ready()) {
        // 준비 안됨 (초기화 중이거나 연결 대기 중)
        // 재초기화가 필요한 경우 처리
        if (mqtt_connect_wait_count == 0 && !mqtt_initialized) {
            // 재초기화 후 대기
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        return ESP_ERR_INVALID_STATE;
    }
    
    // JSON 데이터 생성
    char *json_string = mqtt_create_test_json(ip_address);
    if (json_string == NULL) {
        ESP_LOGE(TAG, "JSON 데이터 생성 실패");
        return ESP_FAIL;
    }
    
    // 데이터 크기 검증
    size_t data_size = strlen(json_string);
    if (!mqtt_validate_data_size(data_size)) {
        free(json_string);
        return ESP_ERR_INVALID_SIZE;
    }
    
    ESP_LOGI(TAG, "전송할 데이터 크기: %zu bytes, 카운트: %d", data_size, mqtt_test_count);

    // 토픽 설정 (동적으로 생성된 토픽 사용)
    const char* topic = mqtt_send_topic;
    if (!mqtt_validate_topic(topic)) {
        free(json_string);
        return ESP_ERR_INVALID_ARG;
    }

    // MQTT로 데이터 전송
    esp_err_t send_ret = mqtt_send_data(topic, json_string);
    
    if (send_ret == ESP_OK) {
        printf("MQTT 데이터 전송 시작 성공 - 크기: %zu bytes, 카운트: %d\n", data_size, mqtt_test_count);
        fflush(stdout);
        ESP_LOGI(TAG, "MQTT 데이터 전송 시작 성공");
        ESP_LOGI(TAG, "  - 데이터 크기: %zu bytes", data_size);
        ESP_LOGI(TAG, "  - 카운트: %d", mqtt_test_count);
        ESP_LOGI(TAG, "  - 브로커 도달 시간은 MQTT_EVENT_PUBLISHED에서 확인됩니다");
    } else {
        printf("MQTT 데이터 전송 실패: %s\n", esp_err_to_name(send_ret));
        fflush(stdout);
        ESP_LOGE(TAG, "MQTT 데이터 전송 실패: %s", esp_err_to_name(send_ret));
    }
    
    // 메모리 해제
    free(json_string);
    
    // MQTT 데이터 수신 처리
    mqtt_receive_data();
    
    return send_ret;
}

// MQTT 데이터 전송 함수 (실제 데이터 전송용)
// 이전에 작성한 JSON 생성 코드가 mqtt_create_test_json()에서 실행됩니다
esp_err_t mqtt_data_send(const char* ip_address)
{
    ESP_LOGI(TAG, "=== mqtt_data_send() 호출됨 ===");
    
    // MQTT 연결 상태 확인
    if (!mqtt_initialized || !mqtt_connected) {
        ESP_LOGE(TAG, "MQTT 미연결 - 데이터 전송 불가");
        return ESP_ERR_INVALID_STATE;
    }
    
    // JSON 데이터 생성 (이전에 작성한 코드가 여기서 실행됩니다)
    ESP_LOGI(TAG, "이전에 작성한 JSON 생성 코드 실행 중...");
    char *json_string = mqtt_create_test_json(ip_address);
    if (json_string == NULL) {
        ESP_LOGE(TAG, "JSON 데이터 생성 실패");
        return ESP_FAIL;
    }
    
    // 데이터 크기 검증
    size_t data_size = strlen(json_string);
    if (!mqtt_validate_data_size(data_size)) {
        free(json_string);
        return ESP_ERR_INVALID_SIZE;
    }
    
    ESP_LOGI(TAG, "전송할 데이터 크기: %zu bytes, 카운트: %d", data_size, mqtt_test_count);

    // 토픽 설정 (동적으로 생성된 토픽 사용)
    const char* topic = mqtt_send_topic;
    if (!mqtt_validate_topic(topic)) {
        free(json_string);
        return ESP_ERR_INVALID_ARG;
    }

    // MQTT로 데이터 전송
    ESP_LOGI(TAG, "MQTT로 데이터 전송 시작...");
    esp_err_t send_ret = mqtt_send_data(topic, json_string);
    
    if (send_ret == ESP_OK) {
        ESP_LOGI(TAG, "MQTT 데이터 전송 시작 성공");
        ESP_LOGI(TAG, "  - 데이터 크기: %zu bytes", data_size);
        ESP_LOGI(TAG, "  - 카운트: %d", mqtt_test_count);
        ESP_LOGI(TAG, "  - 토픽: %s", topic);
    } else {
        ESP_LOGE(TAG, "MQTT 데이터 전송 실패: %s", esp_err_to_name(send_ret));
    }
    
    // 메모리 해제
    free(json_string);
    
    // MQTT 데이터 수신 처리
    mqtt_receive_data();
    
    ESP_LOGI(TAG, "=== mqtt_data_send() 완료 ===");
    
    return send_ret;
}

// MQTT 연결 정보 가져오기
void mqtt_get_connection_info(char* broker_url, size_t url_len, int* port, char* client_id, size_t id_len)
{
    if (broker_url != NULL && url_len > 0) {
        snprintf(broker_url, url_len, "%s", MQTT_BROKER_URL);
    }
    if (port != NULL) {
        *port = MQTT_BROKER_PORT;
    }
    if (client_id != NULL && id_len > 0) {
        snprintf(client_id, id_len, "%s", mqtt_client_id);
    }
}

// MQTT 토픽 정보 가져오기
const char* mqtt_get_topic(void)
{
    return mqtt_send_topic;
}

