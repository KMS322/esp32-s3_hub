#include "device_collector.h"
#include "http_client.h"
#include "mqtt.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "string.h"
#include "stdlib.h"

static const char *TAG = "DEVICE_COLLECTOR";
static char *g_json_work_buf = NULL;
static size_t g_json_work_buf_size = 0;
/* 250개 배치 기준 기본 작업 버퍼 (재사용) */
#define JSON_WORK_BUF_DEFAULT_SIZE (40 * 1024)

static char *get_json_work_buffer(size_t need_size)
{
    if (need_size == 0) {
        return NULL;
    }

    if (g_json_work_buf != NULL && g_json_work_buf_size >= need_size) {
        return g_json_work_buf;
    }

    if (g_json_work_buf != NULL) {
        free(g_json_work_buf);
        g_json_work_buf = NULL;
        g_json_work_buf_size = 0;
    }

    /* PSRAM 있으면 우선 사용, 없으면 내부 힙 fallback */
    g_json_work_buf = (char *)heap_caps_malloc(need_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (g_json_work_buf == NULL) {
        g_json_work_buf = (char *)heap_caps_malloc(need_size, MALLOC_CAP_8BIT);
    }

    if (g_json_work_buf != NULL) {
        g_json_work_buf_size = need_size;
    }
    return g_json_work_buf;
}

static void release_json_work_buffer_if_internal_heap(void)
{
    if (g_json_work_buf == NULL) {
        return;
    }
#if !CONFIG_SPIRAM
    /* PSRAM 비활성화 빌드에서는 내부 힙 점유를 오래 유지하지 않음 (BT/HCI malloc 보호) */
    free(g_json_work_buf);
    g_json_work_buf = NULL;
    g_json_work_buf_size = 0;
#endif
}

/* JSON escape 후 길이 계산 (널 제외) */
static size_t json_escaped_length(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    size_t n = 0;
    while (*s != '\0') {
        unsigned char c = (unsigned char)*s++;
        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') {
            n += 2;  // \" \\ \n \r \t
        } else if (c < 0x20) {
            n += 6;  // \u00XX
        } else {
            n += 1;
        }
    }
    return n;
}

/** 실제 escape 길이 기반 JSON 크기 추정 */
static size_t estimate_json_buffer_size(const device_collector_t* device)
{
    size_t n = 320u;
    for (int i = 0; i < device->data_count; i++) {
        if (device->device_data[i] != NULL) {
            size_t esc_len = json_escaped_length(device->device_data[i]);
            n += esc_len + 4u;  // 따옴표 + 콤마 여유
        }
    }
    n += 128u;
    if (n < 2048u) {
        n = 2048u;
    }
    return n;
}

// 최대 디바이스 개수 (여러 nrf5340 연결 대비)
#define MAX_DEVICES 10

// 디버깅용: 총 데이터 수신 카운터
static int g_total_data_received = 0;

// 제어 문자 제거 함수 (\n, \r, \t 등 제거)
static void remove_control_chars(char* str) {
    if (str == NULL) return;
    
    char* src = str;
    char* dst = str;
    
    while (*src != '\0') {
        // 제어 문자(0x00-0x1F) 제거, 일반 문자만 유지
        if ((unsigned char)*src >= 0x20) {
            *dst++ = *src;
        }
        src++;
    }
    *dst = '\0';
}

// JSON 이스케이프 처리 함수 (제어 문자 처리)
static int json_escape_string(const char* input, char* output, size_t output_size) {
    if (input == NULL || output == NULL || output_size == 0) {
        return 0;
    }
    
    size_t out_pos = 0;
    const char* in = input;
    
    while (*in != '\0' && out_pos < output_size - 1) {
        unsigned char c = (unsigned char)*in;
        
        // 제어 문자 처리 (0x00-0x1F)
        if (c < 0x20) {
            switch (c) {
                case '\n':
                    if (out_pos + 2 < output_size - 1) {
                        output[out_pos++] = '\\';
                        output[out_pos++] = 'n';
                        in++;
                    } else {
                        goto end;
                    }
                    break;
                case '\r':
                    if (out_pos + 2 < output_size - 1) {
                        output[out_pos++] = '\\';
                        output[out_pos++] = 'r';
                        in++;
                    } else {
                        goto end;
                    }
                    break;
                case '\t':
                    if (out_pos + 2 < output_size - 1) {
                        output[out_pos++] = '\\';
                        output[out_pos++] = 't';
                        in++;
                    } else {
                        goto end;
                    }
                    break;
                default:
                    // 다른 제어 문자는 유니코드 이스케이프로 처리
                    if (out_pos + 6 < output_size - 1) {
                        snprintf(output + out_pos, output_size - out_pos, "\\u%04x", c);
                        out_pos += 6;
                        in++;
                    } else {
                        goto end;
                    }
                    break;
            }
        } else if (c == '"') {
            // 따옴표 이스케이프
            if (out_pos + 2 < output_size - 1) {
                output[out_pos++] = '\\';
                output[out_pos++] = '"';
                in++;
            } else {
                goto end;
            }
        } else if (c == '\\') {
            // 백슬래시 이스케이프
            if (out_pos + 2 < output_size - 1) {
                output[out_pos++] = '\\';
                output[out_pos++] = '\\';
                in++;
            } else {
                goto end;
            }
        } else {
            // 일반 문자
            output[out_pos++] = *in++;
        }
    }
    
end:
    output[out_pos] = '\0';
    return out_pos;
}

// 디바이스 수집기 배열
static device_collector_t devices[MAX_DEVICES];
static bool is_initialized = false;

// MAC 주소 비교 함수
static bool mac_address_equal(const uint8_t* mac1, const uint8_t* mac2) {
    return memcmp(mac1, mac2, 6) == 0;
}

// 디바이스 수집기 초기화
esp_err_t device_collector_init(void) {
    if (is_initialized) {
        ESP_LOGW(TAG, "디바이스 수집기가 이미 초기화되어 있습니다");
        return ESP_OK;
    }

    // 모든 디바이스 초기화
    memset(devices, 0, sizeof(devices));
    for (int i = 0; i < MAX_DEVICES; i++) {
        devices[i].is_active = false;
        devices[i].data_count = 0;
        memset(devices[i].mac_address, 0, 6);
        for (int j = 0; j < 50; j++) {
            devices[i].device_data[j] = NULL;
        }
    }

    is_initialized = true;
    /* 내부 힙 압박 방지를 위해 init 시 선할당하지 않음.
     * 업로드 직전에 필요할 때만 할당한다. */
    ESP_LOGI(TAG, "디바이스 수집기 초기화 완료 (최대 %d개 디바이스)", MAX_DEVICES);
    return ESP_OK;
}

// 빈 슬롯 찾기
static int find_empty_slot(void) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!devices[i].is_active) {
            return i;
        }
    }
    return -1; // 슬롯 없음
}

// MAC 주소로 디바이스 찾기
static device_collector_t* find_device_by_mac(const uint8_t* mac_address) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].is_active && mac_address_equal(devices[i].mac_address, mac_address)) {
            return &devices[i];
        }
    }
    return NULL;
}

// MAC 주소를 문자열로 변환
void device_collector_mac_to_string(const uint8_t* mac_address, char* buffer) {
    if (mac_address == NULL || buffer == NULL) {
        return;
    }
    snprintf(buffer, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac_address[0], mac_address[1], mac_address[2],
             mac_address[3], mac_address[4], mac_address[5]);
}

// 연결된 디바이스에 대한 객체 생성
esp_err_t device_collector_create_device(const uint8_t* mac_address) {
    if (mac_address == NULL) {
        ESP_LOGE(TAG, "MAC 주소가 NULL입니다");
        return ESP_ERR_INVALID_ARG;
    }

    if (!is_initialized) {
        ESP_LOGE(TAG, "디바이스 수집기가 초기화되지 않았습니다");
        return ESP_ERR_INVALID_STATE;
    }

    // 이미 존재하는 디바이스인지 확인
    device_collector_t* existing = find_device_by_mac(mac_address);
    if (existing != NULL) {
        char mac_str[18];
        device_collector_mac_to_string(mac_address, mac_str);
        ESP_LOGW(TAG, "디바이스가 이미 존재합니다: %s", mac_str);
        return ESP_OK; // 이미 존재하므로 성공으로 처리
    }

    // 빈 슬롯 찾기
    int slot = find_empty_slot();
    if (slot < 0) {
        ESP_LOGE(TAG, "디바이스 슬롯이 가득 찼습니다 (최대 %d개)", MAX_DEVICES);
        return ESP_ERR_NO_MEM;
    }

    // 디바이스 초기화
    device_collector_t* device = &devices[slot];
    memcpy(device->mac_address, mac_address, 6);
    device->data_count = 0;
    device->is_active = true;
    device->has_start_time = false;
    device->overflow_warned = false;

    // 메타데이터 초기화
    device->hr = 0;
    device->spo2 = 0;
    device->temp = 0.0f;
    device->battery = 0;
    device->gyro = 0;
    memset(device->start_time, 0, sizeof(device->start_time));

    // device_data 배열 초기화 (250개로 변경)
    for (int i = 0; i < 250; i++) {
        device->device_data[i] = NULL;
    }

    char mac_str[18];
    device_collector_mac_to_string(mac_address, mac_str);
    // ESP_LOGI(TAG, "=== 디바이스 객체 생성 완료 ===");
    // ESP_LOGI(TAG, "MAC 주소: %s", mac_str);
    // ESP_LOGI(TAG, "슬롯 인덱스: %d", slot);
    // ESP_LOGI(TAG, "=============================");

    return ESP_OK;
}

// 특정 디바이스에 데이터 추가
esp_err_t device_collector_add_data(const uint8_t* mac_address, const char* data) {
    if (mac_address == NULL || data == NULL) {
        ESP_LOGE(TAG, "MAC 주소 또는 데이터가 NULL입니다");
        return ESP_ERR_INVALID_ARG;
    }

    if (!is_initialized) {
        ESP_LOGE(TAG, "디바이스 수집기가 초기화되지 않았습니다");
        return ESP_ERR_INVALID_STATE;
    }

    // 디바이스 찾기
    device_collector_t* device = find_device_by_mac(mac_address);
    if (device == NULL) {
        char mac_str[18];
        device_collector_mac_to_string(mac_address, mac_str);
        ESP_LOGW(TAG, "디바이스를 찾을 수 없습니다: %s", mac_str);
        return ESP_ERR_NOT_FOUND;
    }

    char mac_str[18];
    device_collector_mac_to_string(mac_address, mac_str);

    /* 이번 호출에서 메타데이터(쉼표 4개) 파싱에 성공했을 때만 MQTT 업로드 (센서만 온 패킷과 구분) */
    bool do_mqtt_upload = false;

    // 데이터 복사본 생성 (제어 문자 제거)
    int data_len = strlen(data);
    char* data_copy = (char*)malloc(data_len + 1);
    if (data_copy == NULL) {
        ESP_LOGE(TAG, "메모리 할당 실패");
        return ESP_ERR_NO_MEM;
    }
    strcpy(data_copy, data);
    remove_control_chars(data_copy);

    // 쉼표 개수로 데이터 타입 판별
    int comma_count = 0;
    for (const char* p = data_copy; *p; p++) {
        if (*p == ',') comma_count++;
    }

    // 첫 데이터 수신 시 타임스탬프 기록
    if (!device->has_start_time && device->data_count == 0) {
        // 현재 시각을 HHmmssSSS 형식으로 저장
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm timeinfo;
        localtime_r(&tv.tv_sec, &timeinfo);

        snprintf(device->start_time, sizeof(device->start_time),
                 "%02d%02d%02d%03d",
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                 (int)(tv.tv_usec / 1000));
        device->has_start_time = true;
        ESP_LOGI(TAG, "[%s] 첫 데이터 수신 - start_time: %s", mac_str, device->start_time);
    }

    if (comma_count == 2) {
        // 센서 데이터 (IR,RED,GREEN) - 메타데이터 올 때까지 계속 수집
        g_total_data_received++;  // 디버깅용 카운터

        if (device->data_count < 250) {
            device->device_data[device->data_count] = data_copy;
            device->data_count++;
        } else {
            // 250개 초과 시 한 번만 경고 (매 패킷마다 로그하지 않음)
            if (!device->overflow_warned) {
                device->overflow_warned = true;
                ESP_LOGW(TAG, "[%s] 센서 데이터가 250개를 초과했습니다 (%d개). 메타데이터 대기 중...",
                         mac_str, device->data_count);
            }
            free(data_copy);
        }
    } else if (comma_count == 3) {
        // 센서 데이터 with cnt (cnt,IR,RED,GREEN) - 테스트용
        // cnt는 디버깅용이므로 그대로 저장 (서버에서 파싱)
        g_total_data_received++;  // 디버깅용 카운터

        if (device->data_count < 250) {
            device->device_data[device->data_count] = data_copy;
            device->data_count++;
        } else {
            // 250개 초과 시 한 번만 경고 (매 패킷마다 로그하지 않음)
            if (!device->overflow_warned) {
                device->overflow_warned = true;
                ESP_LOGW(TAG, "[%s] 센서 데이터가 250개를 초과했습니다 (%d개). 메타데이터 대기 중...",
                         mac_str, device->data_count);
            }
            free(data_copy);
        }
    } else if (comma_count == 4) {
        // 메타데이터: HR, Spo2, Temp, Battery, Gyro (예: "84,98,36.5,100,0")
        ESP_LOGI(TAG, "[%s] 메타데이터 수신 (센서 데이터 %d개 수집됨, 총 수신: %d개): %s",
                 mac_str, device->data_count, g_total_data_received, data_copy);

        // 카운터 리셋
        g_total_data_received = 0;

        if (sscanf(data_copy, "%d,%d,%f,%d,%d",
                  &device->hr,
                  &device->spo2,
                  &device->temp,
                  &device->battery,
                  &device->gyro) == 5) {
            do_mqtt_upload = true;
            ESP_LOGI(TAG, "[%s] 메타데이터 파싱 성공", mac_str);
            ESP_LOGI(TAG, "  hr: %d", device->hr);
            ESP_LOGI(TAG, "  spo2: %d", device->spo2);
            ESP_LOGI(TAG, "  temp: %.1f", device->temp);
            ESP_LOGI(TAG, "  battery: %d", device->battery);
            ESP_LOGI(TAG, "  gyro: %d", device->gyro);
        } else {
            ESP_LOGE(TAG, "[%s] 메타데이터 파싱 실패: %s", mac_str, data_copy);
            free(data_copy);
            return ESP_ERR_INVALID_ARG;
        }
        free(data_copy);
    } else {
        ESP_LOGW(TAG, "[%s] 알 수 없는 데이터 형식 (쉼표 개수: %d): %s", mac_str, comma_count, data_copy);
        free(data_copy);
        return ESP_ERR_INVALID_ARG;
    }

    // 메타데이터 라인 파싱 성공 시에만 업로드 (센서만 있는 패킷은 여기서 보내지 않음)
    if (do_mqtt_upload && device->data_count > 0) {
        ESP_LOGI(TAG, "=== [%s] 데이터 %d개 + 메타데이터 수집 완료! ===", mac_str, device->data_count);

        // 1단계: JSON 생성 — 필요 바이트만 요청 (항목당 259바이트 가정은 249개 시 ~64KB로 과도함)
        size_t json_size = estimate_json_buffer_size(device);
        char* json_data = get_json_work_buffer(json_size);

        if (json_data == NULL) {
            ESP_LOGE(TAG, "[%s] JSON 버퍼 할당 실패 (need %zu B, free_heap=%u, min_free=%u)",
                     mac_str, json_size,
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size());
        } else {
            // JSON: HR, Spo2, Temp(소수 첫째자리), Battery, Gyro + data 배열 + start_time
            int offset = snprintf(json_data, json_size,
                                 "{\"device_mac_address\":\"%s\","
                                 "\"hr\":%d,"
                                 "\"spo2\":%d,"
                                 "\"temp\":%.1f,"
                                 "\"battery\":%d,"
                                 "\"gyro\":%d,"
                                 "\"data\":[",
                                 mac_str,
                                 device->hr,
                                 device->spo2,
                                 device->temp,
                                 device->battery,
                                 device->gyro);

            // 실제 수집된 센서 데이터를 배열에 추가
            for (int i = 0; i < device->data_count; i++) {
                if (device->device_data[i] != NULL) {
                    if (i > 0) {
                        offset += snprintf(json_data + offset, json_size - offset, ",");
                    }

                    // 따옴표와 백슬래시만 이스케이프 처리 (제어 문자는 이미 제거됨)
                    if (strchr(device->device_data[i], '"') != NULL || strchr(device->device_data[i], '\\') != NULL ||
                        strchr(device->device_data[i], '\n') != NULL || strchr(device->device_data[i], '\r') != NULL ||
                        strchr(device->device_data[i], '\t') != NULL) {
                        // 따옴표나 백슬래시가 있으면 이스케이프 필요
                        size_t escaped_buf_size = json_escaped_length(device->device_data[i]) + 1;
                        char* escaped_str = (char*)malloc(escaped_buf_size);
                        if (escaped_str != NULL) {
                            json_escape_string(device->device_data[i], escaped_str, escaped_buf_size);
                            offset += snprintf(json_data + offset, json_size - offset, "\"%s\"", escaped_str);
                            free(escaped_str);
                        } else {
                            // 메모리 할당 실패 시 원본 사용 (로그 남기고 진행)
                            ESP_LOGW(TAG, "[%s] 이스케이프 버퍼 할당 실패, 원본 사용", mac_str);
                            offset += snprintf(json_data + offset, json_size - offset, "\"%s\"",
                                             device->device_data[i]);
                        }
                    } else {
                        // 따옴표와 백슬래시가 없으면 그대로 사용 (제어 문자는 이미 제거됨)
                        offset += snprintf(json_data + offset, json_size - offset, "\"%s\"",
                                         device->device_data[i]);
                    }
                }
            }

            // start_time 추가 및 JSON 종료
            offset += snprintf(json_data + offset, json_size - offset,
                             "],\"start_time\":\"%s\"}", device->start_time);

            // 2단계: MQTT 전송 (JSON 생성 완료 후)
            ESP_LOGI(TAG, "[%s] JSON 생성 완료 (%d bytes, 데이터 %d개)", mac_str, offset, device->data_count);

            if (mqtt_is_connected()) {
                const char* mqtt_topic = mqtt_get_topic();

                esp_err_t mqtt_ret = mqtt_send_data(mqtt_topic, json_data);
                if (mqtt_ret == ESP_OK) {
                    ESP_LOGI(TAG, "[%s] MQTT 전송 성공 - 토픽: %s, data.length: %d",
                             mac_str, mqtt_topic, device->data_count);
                } else {
                    ESP_LOGE(TAG, "[%s] MQTT 전송 실패: %s", mac_str, esp_err_to_name(mqtt_ret));
                }
            } else {
                ESP_LOGW(TAG, "[%s] MQTT 미연결 - MQTT 전송 건너뜀", mac_str);
            }

            /* PSRAM OFF에서는 내부 힙을 즉시 반환해 BT 메모리 고갈을 방지 */
            release_json_work_buffer_if_internal_heap();
        }

        // 3단계: 버퍼 초기화 (MQTT 전송 완료 후)
        // 새로운 데이터가 들어와도 이전 사이클과 명확히 구분됨
        int sent_data_count = device->data_count;

        for (int i = 0; i < 250; i++) {
            if (device->device_data[i] != NULL) {
                free(device->device_data[i]);
                device->device_data[i] = NULL;
            }
        }
        device->data_count = 0;
        device->overflow_warned = false;

        // 메타데이터 초기화
        device->hr = 0;
        device->spo2 = 0;
        device->temp = 0.0f;
        device->battery = 0;
        device->gyro = 0;
        device->has_start_time = false;
        memset(device->start_time, 0, sizeof(device->start_time));

        ESP_LOGI(TAG, "[%s] 전송 완료 및 버퍼 초기화 (전송 데이터: %d개, 다음 수집 준비)",
                 mac_str, sent_data_count);
    }

    return ESP_OK;
}

// 디바이스 객체 제거 (연결 해제 시)
esp_err_t device_collector_remove_device(const uint8_t* mac_address) {
    if (mac_address == NULL) {
        ESP_LOGE(TAG, "MAC 주소가 NULL입니다");
        return ESP_ERR_INVALID_ARG;
    }

    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // 디바이스 찾기
    device_collector_t* device = find_device_by_mac(mac_address);
    if (device == NULL) {
        char mac_str[18];
        device_collector_mac_to_string(mac_address, mac_str);
        ESP_LOGW(TAG, "제거할 디바이스를 찾을 수 없습니다: %s", mac_str);
        return ESP_ERR_NOT_FOUND;
    }

    // 데이터 메모리 해제
    for (int i = 0; i < device->data_count; i++) {
        if (device->device_data[i] != NULL) {
            free(device->device_data[i]);
            device->device_data[i] = NULL;
        }
    }

    char mac_str[18];
    device_collector_mac_to_string(mac_address, mac_str);
    ESP_LOGI(TAG, "=== 디바이스 객체 제거 ===");
    ESP_LOGI(TAG, "MAC 주소: %s", mac_str);
    ESP_LOGI(TAG, "제거된 데이터 개수: %d", device->data_count);

    // 디바이스 초기화
    device->is_active = false;
    device->data_count = 0;
    memset(device->mac_address, 0, 6);
    memset(device->device_data, 0, sizeof(device->device_data));

    ESP_LOGI(TAG, "=========================");

    return ESP_OK;
}

