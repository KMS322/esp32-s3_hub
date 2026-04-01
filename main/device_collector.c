#include "device_collector.h"
#include "http_client.h"
#include "mqtt.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "stdarg.h"
#include "string.h"
#include "stdlib.h"

static const char *TAG = "DEVICE_COLLECTOR";
static char *g_json_work_buf = NULL;
/* 250개 배치 JSON 고정 작업 버퍼 (동적 추정/가변 할당 제거) */
#define JSON_FIXED_BUF_SIZE (36 * 1024)
// 최대 디바이스 개수 (여러 nrf5340 연결 대비)
#define MAX_DEVICES 5
#define DEVICE_SAMPLE_MAX 250
#define DEVICE_SAMPLE_STR_MAX 64

static bool json_appendf(char *buf, size_t cap, size_t *off, const char *fmt, ...)
{
    if (buf == NULL || off == NULL || *off >= cap) {
        return false;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= (cap - *off)) {
        return false;
    }
    *off += (size_t)n;
    return true;
}

static bool json_append_quoted_escaped(char *buf, size_t cap, size_t *off, const char *src)
{
    if (!json_appendf(buf, cap, off, "\"")) {
        return false;
    }
    if (src != NULL) {
        while (*src != '\0') {
            unsigned char c = (unsigned char)(*src++);
            if (c == '"' || c == '\\') {
                if (!json_appendf(buf, cap, off, "\\%c", c)) return false;
            } else if (c == '\n') {
                if (!json_appendf(buf, cap, off, "\\n")) return false;
            } else if (c == '\r') {
                if (!json_appendf(buf, cap, off, "\\r")) return false;
            } else if (c == '\t') {
                if (!json_appendf(buf, cap, off, "\\t")) return false;
            } else if (c < 0x20) {
                if (!json_appendf(buf, cap, off, "\\u%04x", c)) return false;
            } else {
                if (!json_appendf(buf, cap, off, "%c", c)) return false;
            }
        }
    }
    return json_appendf(buf, cap, off, "\"");
}

static void reset_device_batch_buffers(device_collector_t *device)
{
    if (device == NULL) {
        return;
    }
    for (int i = 0; i < DEVICE_SAMPLE_MAX; i++) {
        if (device->device_data[i] != NULL) {
            device->device_data[i][0] = '\0';
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
}

// 디버깅용: 총 데이터 수신 카운터
static int g_total_data_received = 0;
/* 고정 슬롯 힙 풀: 런타임 1회 할당(반복 malloc/free 제거 + .bss 절감) */
static char *g_sample_pool = NULL;

static inline char *sample_slot_ptr(int dev_idx, int sample_idx)
{
    size_t index = ((size_t)dev_idx * DEVICE_SAMPLE_MAX + (size_t)sample_idx) * DEVICE_SAMPLE_STR_MAX;
    return &g_sample_pool[index];
}

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

    size_t pool_size = (size_t)MAX_DEVICES * DEVICE_SAMPLE_MAX * DEVICE_SAMPLE_STR_MAX;
    if (g_sample_pool == NULL) {
#if CONFIG_SPIRAM
        g_sample_pool = (char *)heap_caps_malloc(pool_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
        if (g_sample_pool == NULL) {
            g_sample_pool = (char *)heap_caps_malloc(pool_size, MALLOC_CAP_8BIT);
        }
        if (g_sample_pool == NULL) {
            ESP_LOGE(TAG, "샘플 풀 할당 실패 (%u bytes)", (unsigned)pool_size);
            return ESP_ERR_NO_MEM;
        }
        memset(g_sample_pool, 0, pool_size);
    }

    for (int i = 0; i < MAX_DEVICES; i++) {
        devices[i].is_active = false;
        devices[i].data_count = 0;
        memset(devices[i].mac_address, 0, 6);
        for (int j = 0; j < DEVICE_SAMPLE_MAX; j++) {
            char *slot = sample_slot_ptr(i, j);
            slot[0] = '\0';
            devices[i].device_data[j] = slot;
        }
    }

    is_initialized = true;
    /* 고정 버퍼 1회 선할당: 실행 중 가변 할당/파편화 최소화 */
    if (g_json_work_buf == NULL) {
#if CONFIG_SPIRAM
        g_json_work_buf = (char *)heap_caps_malloc(JSON_FIXED_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
        if (g_json_work_buf == NULL) {
            g_json_work_buf = (char *)heap_caps_malloc(JSON_FIXED_BUF_SIZE, MALLOC_CAP_8BIT);
        }
    }
    if (g_json_work_buf == NULL) {
        ESP_LOGW(TAG, "JSON 고정 버퍼 선할당 실패(%d B), 런타임 전송 실패 가능", JSON_FIXED_BUF_SIZE);
    }
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

    // device_data 배열 초기화 (고정 풀 슬롯과 연결)
    for (int i = 0; i < DEVICE_SAMPLE_MAX; i++) {
        char *p = sample_slot_ptr(slot, i);
        p[0] = '\0';
        device->device_data[i] = p;
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
    char data_copy[DEVICE_SAMPLE_STR_MAX];
    strncpy(data_copy, data, sizeof(data_copy) - 1);
    data_copy[sizeof(data_copy) - 1] = '\0';
    remove_control_chars(data_copy);

    // 쉼표 개수로 데이터 타입 판별
    int comma_count = 0;
    for (const char* p = data_copy; *p; p++) {
        if (*p == ',') comma_count++;
    }

    // 첫 데이터 수신 시 타임스탬프 기록 (MQTT JSON start_time: YYMMDD-HHmmssSSS)
    if (!device->has_start_time && device->data_count == 0) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm timeinfo;
        localtime_r(&tv.tv_sec, &timeinfo);

        int y2 = (timeinfo.tm_year + 1900) % 100;
        if (y2 < 0) {
            y2 = 0;
        }
        int mo = timeinfo.tm_mon + 1;
        if (mo < 1) {
            mo = 1;
        }
        if (mo > 12) {
            mo = 12;
        }
        int dd = timeinfo.tm_mday;
        if (dd < 1) {
            dd = 1;
        }
        if (dd > 31) {
            dd = 31;
        }
        int hh = timeinfo.tm_hour % 24;
        if (hh < 0) {
            hh += 24;
        }
        int mi = timeinfo.tm_min % 60;
        if (mi < 0) {
            mi += 60;
        }
        int sc = timeinfo.tm_sec % 60;
        if (sc < 0) {
            sc += 60;
        }
        unsigned msec = (unsigned)(tv.tv_usec / 1000);
        if (msec > 999u) {
            msec = 999u;
        }
        /* GCC -Wformat-truncation: tm_* 타입이 int라 정적 분석이 과대평가 — 실제 출력은 16+NUL */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
        snprintf(device->start_time, sizeof(device->start_time),
                 "%02d%02d%02d-%02d%02d%02d%03u",
                 y2, mo, dd, hh, mi, sc, msec);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        device->has_start_time = true;
        ESP_LOGI(TAG, "[%s] 첫 데이터 수신 - start_time: %s", mac_str, device->start_time);
    }

    if (comma_count == 2) {
        // 센서 데이터 (IR,RED,GREEN) - 메타데이터 올 때까지 계속 수집
        g_total_data_received++;  // 디버깅용 카운터

        if (device->data_count < DEVICE_SAMPLE_MAX) {
            strncpy(device->device_data[device->data_count], data_copy, DEVICE_SAMPLE_STR_MAX - 1);
            device->device_data[device->data_count][DEVICE_SAMPLE_STR_MAX - 1] = '\0';
            device->data_count++;
        } else {
            // 250개 초과 시 한 번만 경고 (매 패킷마다 로그하지 않음)
            if (!device->overflow_warned) {
                device->overflow_warned = true;
                ESP_LOGW(TAG, "[%s] 센서 데이터가 250개를 초과했습니다 (%d개). 메타데이터 대기 중...",
                         mac_str, device->data_count);
            }
        }
    } else if (comma_count == 3) {
        // 센서 데이터 with cnt (cnt,IR,RED,GREEN) - 테스트용
        // cnt는 디버깅용이므로 그대로 저장 (서버에서 파싱)
        g_total_data_received++;  // 디버깅용 카운터

        if (device->data_count < DEVICE_SAMPLE_MAX) {
            strncpy(device->device_data[device->data_count], data_copy, DEVICE_SAMPLE_STR_MAX - 1);
            device->device_data[device->data_count][DEVICE_SAMPLE_STR_MAX - 1] = '\0';
            device->data_count++;
        } else {
            // 250개 초과 시 한 번만 경고 (매 패킷마다 로그하지 않음)
            if (!device->overflow_warned) {
                device->overflow_warned = true;
                ESP_LOGW(TAG, "[%s] 센서 데이터가 250개를 초과했습니다 (%d개). 메타데이터 대기 중...",
                         mac_str, device->data_count);
            }
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
            if (device->has_start_time && device->start_time[0] != '\0') {
                ESP_LOGI(TAG, "  start_time(MQTT JSON): %s", device->start_time);
            } else {
                ESP_LOGW(TAG, "  start_time: (미설정)");
            }
        } else {
            ESP_LOGE(TAG, "[%s] 메타데이터 파싱 실패: %s", mac_str, data_copy);
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        ESP_LOGW(TAG, "[%s] 알 수 없는 데이터 형식 (쉼표 개수: %d): %s", mac_str, comma_count, data_copy);
        return ESP_ERR_INVALID_ARG;
    }

    // 메타데이터 라인 파싱 성공 시에만 업로드 (센서만 있는 패킷은 여기서 보내지 않음)
    if (do_mqtt_upload && device->data_count > 0) {
        ESP_LOGI(TAG, "=== [%s] 데이터 %d개 + 메타데이터 수집 완료! ===", mac_str, device->data_count);
        bool batch_reset = false;
        int sent_data_count = device->data_count;
        /* reset_device_batch_buffers()가 MQTT 전에 start_time을 지우므로 발행 로그용 복사 */
        char mqtt_start_time_snap[20] = {0};

        // 1단계: JSON 생성 — 고정 버퍼 사용
        char* json_data = g_json_work_buf;
        size_t json_size = JSON_FIXED_BUF_SIZE;
        if (json_data == NULL) {
            ESP_LOGE(TAG, "[%s] JSON 고정 버퍼 없음 (size=%zu B, free_heap=%u, min_free=%u)",
                     mac_str, json_size,
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size());
        } else {
            // JSON: HR, Spo2, Temp(소수 첫째자리), Battery, Gyro + data 배열 + start_time(YYMMDD-HHmmssSSS)
            size_t offset = 0;
            bool json_ok = json_appendf(json_data, json_size, &offset,
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
            for (int i = 0; i < device->data_count && json_ok; i++) {
                if (device->device_data[i] != NULL && device->device_data[i][0] != '\0') {
                    if (i > 0) {
                        json_ok = json_appendf(json_data, json_size, &offset, ",");
                    }
                    if (json_ok) {
                        json_ok = json_append_quoted_escaped(json_data, json_size, &offset, device->device_data[i]);
                    }
                }
            }

            // start_time 추가 및 JSON 종료
            if (json_ok) {
                json_ok = json_appendf(json_data, json_size, &offset, "],\"start_time\":\"%s\"}", device->start_time);
            }
            if (!json_ok) {
                ESP_LOGE(TAG, "[%s] JSON 고정 버퍼 초과 (cap=%zu, data_count=%d)", mac_str, json_size, device->data_count);
            }

            // 2단계: JSON 생성 완료 후, BT 힙 보호를 위해 샘플 버퍼를 먼저 반환
            ESP_LOGI(TAG, "[%s] JSON 생성 완료 (%u bytes, 데이터 %d개, start_time=%s)", mac_str,
                     (unsigned)offset, sent_data_count,
                     (device->start_time[0] != '\0') ? device->start_time : "-");
            if (json_ok) {
                strncpy(mqtt_start_time_snap, device->start_time, sizeof(mqtt_start_time_snap) - 1);
                mqtt_start_time_snap[sizeof(mqtt_start_time_snap) - 1] = '\0';
                reset_device_batch_buffers(device);
                batch_reset = true;
            }

            // 3단계: MQTT 전송
            if (json_ok && mqtt_is_connected()) {
                const char* mqtt_topic = mqtt_get_topic();

                esp_err_t mqtt_ret = mqtt_send_data(mqtt_topic, json_data);
                if (mqtt_ret == ESP_OK) {
                    ESP_LOGI(TAG, "[%s] MQTT 전송 성공 - 토픽: %s, data.length: %d, start_time: %s",
                             mac_str, mqtt_topic, sent_data_count,
                             (mqtt_start_time_snap[0] != '\0') ? mqtt_start_time_snap : "-");
                } else {
                    ESP_LOGE(TAG, "[%s] MQTT 전송 실패: %s", mac_str, esp_err_to_name(mqtt_ret));
                }
            } else {
                if (!json_ok) {
                    ESP_LOGW(TAG, "[%s] JSON 생성 실패 - MQTT 전송 건너뜀", mac_str);
                } else {
                    ESP_LOGW(TAG, "[%s] MQTT 미연결 - MQTT 전송 건너뜀", mac_str);
                }
            }
        }

        // 4단계: 버퍼 초기화 (JSON 생성 실패 케이스 정리)
        // 새로운 데이터가 들어와도 이전 사이클과 명확히 구분됨
        if (!batch_reset && sent_data_count > 0) {
            reset_device_batch_buffers(device);
        }

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

    int slot = (int)(device - devices);

    char mac_str[18];
    device_collector_mac_to_string(mac_address, mac_str);
    ESP_LOGI(TAG, "=== 디바이스 객체 제거 ===");
    ESP_LOGI(TAG, "MAC 주소: %s", mac_str);
    ESP_LOGI(TAG, "제거된 데이터 개수: %d", device->data_count);

    // 디바이스 초기화 및 해당 슬롯 풀 문자열 비우기
    device->is_active = false;
    device->data_count = 0;
    memset(device->mac_address, 0, 6);
    for (int i = 0; i < DEVICE_SAMPLE_MAX; i++) {
        char *p = sample_slot_ptr(slot, i);
        p[0] = '\0';
        device->device_data[i] = p;
    }

    ESP_LOGI(TAG, "=========================");

    return ESP_OK;
}

