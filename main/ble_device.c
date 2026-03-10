#include "ble_device.h"
#include "device_collector.h"
#include "gpio_control.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "string.h"
#include <ctype.h>
#include <inttypes.h>

static const char *TAG = "BLE_DEVICE";

// BLE 초기화 상태
static bool ble_device_is_init = false;

// 스캔 결과 저장 (전역 변수 - 단일 연결용)
uint8_t found_device_mac[6] = {0};
bool device_found = false;

// 다중 연결용 디바이스 리스트
#define MAX_SCAN_DEVICES 10
typedef struct {
    uint8_t mac_address[6];
    bool is_used;
} scanned_device_t;

static scanned_device_t scanned_devices[MAX_SCAN_DEVICES];
static int scanned_device_count = 0;

// 받은 BLE 데이터 저장
static uint8_t received_data[256] = {0};
static uint16_t received_data_len = 0;

// 내부 변수들
static char target_device_name[32] = {0};
static uint8_t target_mac_address[6] = {0};
static bool target_mac_set = false;

// MAC 주소 배열 필터링용 변수
#define MAX_TARGET_MACS 10
static uint8_t target_mac_list[MAX_TARGET_MACS][6] = {0};
static int target_mac_list_count = 0;

static bool is_scanning = false;
static bool is_connecting = false;
static bool is_connected = false;
static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;

// NUS UUID (Nordic UART Service)
// Service UUID: 6e400001-b5a3-f393-e0a9-e50e24dcca9e
// ESP-IDF는 UUID를 리틀 엔디안 바이트 순서로 저장합니다
// 6e400001-b5a3-f393-e0a9-e50e24dcca9e를 바이트 배열로 변환하면:
// [9e, ca, dc, 24, 0e, e5, a9, e0, 93, f3, a3, b5, 01, 00, 40, 6e]
// NUS Service UUID (현재는 전체 탐색 사용, 필요시 사용 가능)
__attribute__((unused)) static const esp_bt_uuid_t NUS_SERVICE_UUID = {
    .len = ESP_UUID_LEN_128,
    .uuid = { .uuid128 = { 0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e } }
};

// RX Characteristic UUID (Write): 6e400002-b5a3-f393-e0a9-e50e24dcca9e
static const esp_bt_uuid_t NUS_RX_CHAR_UUID = {
    .len = ESP_UUID_LEN_128,
    .uuid = { .uuid128 = { 0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e } }
};

// TX Characteristic UUID (Notify): 6e400003-b5a3-f393-e0a9-e50e24dcca9e
static const esp_bt_uuid_t NUS_TX_CHAR_UUID = {
    .len = ESP_UUID_LEN_128,
    .uuid = { .uuid128 = { 0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x03,0x00,0x40,0x6e } }
};

// 연결 컨텍스트 구조체 (다중 연결 지원)
typedef struct {
    uint16_t conn_id;
    esp_bd_addr_t remote_bda;
    uint16_t service_start;
    uint16_t service_end;
    uint16_t rx_char_handle;
    uint16_t tx_char_handle;
    uint16_t cccd_handle;
    uint8_t rx_char_properties;  // RX characteristic의 properties 저장
    bool is_connected;
    bool is_connecting;
} device_connection_t;

// 최대 연결 수 (ESP32-S3는 최대 8개 정도)
#define MAX_CONNECTIONS 10

// 연결 관리 배열
static device_connection_t connections[MAX_CONNECTIONS];
static int active_connections = 0;

// 기존 전역 변수 (하위 호환성)
static uint16_t g_conn_id = 0;
static esp_bd_addr_t g_remote_bda = {0};
static uint16_t g_service_start = 0;
static uint16_t g_service_end = 0;
static uint16_t g_rx_char_handle = 0;
static uint16_t g_cccd_handle = 0;
static uint16_t g_tx_char_handle = 0;

// 전방 선언
static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);

// 연결 관리 함수
static device_connection_t* find_connection_by_conn_id(uint16_t conn_id);
static device_connection_t* find_connection_by_mac(const uint8_t* mac_address);
static device_connection_t* allocate_connection(void);
static void remove_connection(uint16_t conn_id);

// 기본 Notify 콜백 (약한 심볼) - 사용자가 재정의할 수 있음
__attribute__((weak)) void ble_device_on_notify(const uint8_t* data, uint16_t length) {
    if (data == NULL || length == 0) {
        ESP_LOGI(TAG, "Notify 수신: (빈 데이터)");
        return;
    }
    
    // 데이터를 null-terminated 문자열로 변환
    char data_str[257] = {0}; // 최대 256바이트 + null terminator
    int copy_len = (length < sizeof(data_str) - 1) ? length : sizeof(data_str) - 1;
    memcpy(data_str, data, copy_len);
    data_str[copy_len] = '\0';
    
    // 원본 바이트를 그대로 문자열로 출력 (NULL 종단 없이 안전하게)
    ESP_LOGI(TAG, "%02x:%02x:%02x:%02x:%02x:%02x,%.*s",
             g_remote_bda[0], g_remote_bda[1], g_remote_bda[2],
             g_remote_bda[3], g_remote_bda[4], g_remote_bda[5],
             (int)length, (const char*)data);
    
    // 디바이스 수집기에 데이터 추가
    esp_err_t add_ret = device_collector_add_data(g_remote_bda, data_str);
    if (add_ret != ESP_OK) {
        char mac_str[18];
        device_collector_mac_to_string(g_remote_bda, mac_str);
        ESP_LOGW(TAG, "[%s] 데이터 추가 실패: %s", mac_str, esp_err_to_name(add_ret));
    }
}

// 다중 연결 지원 Notify 콜백 (MAC 주소 파라미터 포함)
// 디버깅용 카운터 (각 디바이스별로 BLE 수신 횟수 추적)
static int g_ble_receive_count[MAX_CONNECTIONS] = {0};

__attribute__((weak)) void ble_device_on_notify_with_mac(const uint8_t* mac_address, const uint8_t* data, uint16_t length) {
    if (data == NULL || length == 0 || mac_address == NULL) {
        ESP_LOGI(TAG, "Notify 수신: (빈 데이터 또는 MAC 없음)");
        return;
    }

    // MAC 주소 문자열로 변환
    char mac_str[18];
    device_collector_mac_to_string(mac_address, mac_str);

    // 데이터를 null-terminated 문자열로 변환
    char data_str[257] = {0}; // 최대 256바이트 + null terminator
    int copy_len = (length < sizeof(data_str) - 1) ? length : sizeof(data_str) - 1;
    memcpy(data_str, data, copy_len);
    data_str[copy_len] = '\0';

    // nRF5340에서 수신한 데이터를 콘솔에 출력 (블로킹 최소화)
    // ESP_LOG_BUFFER_HEX는 블로킹이 심하므로 제거하고 간단한 텍스트만 출력
    // ESP_LOGI(TAG, "[BLE 수신] MAC: %s, 길이: %d bytes, 데이터: %.*s",
    //         mac_str, length, (int)length, (const char*)data);

    // 디버깅용: BLE 수신 카운트 (첫 번째 연결만 추적)
    static bool count_started = false;
    if (!count_started) {
        g_ble_receive_count[0]++;
    }

    // 디바이스 수집기에 데이터 추가 (MAC 주소로 구분)
    esp_err_t add_ret = device_collector_add_data(mac_address, data_str);
    if (add_ret != ESP_OK) {
        ESP_LOGW(TAG, "[%s] 데이터 추가 실패: %s", mac_str, esp_err_to_name(add_ret));
    }
}

// MAC 주소 문자열을 바이트 배열로 변환
static bool parse_mac_address(const char* mac_str, uint8_t* mac_bytes) {
    if (mac_str == NULL || strlen(mac_str) != 17) {
        return false;
    }
    
    // 형식: "AA:BB:CC:DD:EE:FF"
    const char* pos = mac_str;
    for (int i = 0; i < 6; i++) {
        // 16진수 문자 확인
        if (!isxdigit((unsigned char)pos[0]) || !isxdigit((unsigned char)pos[1])) {
            return false;
        }
        
        // 변환
        char hex_str[3] = {(char)pos[0], (char)pos[1], '\0'};
        mac_bytes[i] = (uint8_t)strtol(hex_str, NULL, 16);
        
        // 콜론 확인 (마지막 제외)
        if (i < 5) {
            if (pos[2] != ':') {
                return false;
            }
            pos += 3;
        } else {
            pos += 2;
        }
    }
    
    return true;
}

// GATT 클라이언트 이벤트 핸들러
static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) {
    switch (event) {
        case ESP_GATTC_REG_EVT:
            ESP_LOGI(TAG, "GATT 클라이언트 등록 완료");
            s_gattc_if = gattc_if;
            break;
            
        case ESP_GATTC_OPEN_EVT:
            {
                uint16_t conn_id = param->open.conn_id;
                ESP_LOGI(TAG, "BLE OPEN 이벤트 - status: %d, conn_id: %d", param->open.status, conn_id);
                
                // remote_bda는 OPEN 이벤트에 없으므로 CONNECT 이벤트에서 설정
                // 여기서는 연결 중인 슬롯만 찾기
                
                if (param->open.status == ESP_GATT_OK) {
                    ESP_LOGI(TAG, "BLE 연결 시도 성공 (conn_id=%d), 연결 완료 대기 중", conn_id);
                    // is_connected는 ESP_GATTC_CONNECT_EVT에서 설정
                } else {
                    ESP_LOGE(TAG, "BLE 연결 시도 실패 (conn_id=%d): %d", conn_id, param->open.status);
                    // 연결 실패한 슬롯 찾아서 해제
                    for (int i = 0; i < MAX_CONNECTIONS; i++) {
                        if (connections[i].is_connecting && connections[i].conn_id == conn_id) {
                            connections[i].is_connecting = false;
                            break;
                        }
                    }
                    // 하위 호환성
                    is_connecting = false;
                }
            }
            break;
            
        case ESP_GATTC_CONNECT_EVT:
            ESP_LOGI(TAG, "디바이스 연결됨 - conn_id: %d", param->connect.conn_id);
            
            // 연결 컨텍스트 찾기 또는 생성
            device_connection_t* conn = find_connection_by_mac(param->connect.remote_bda);
            if (conn == NULL) {
                // 연결 중인 슬롯 찾기
                for (int i = 0; i < MAX_CONNECTIONS; i++) {
                    if (connections[i].is_connecting && 
                        memcmp(connections[i].remote_bda, param->connect.remote_bda, 6) == 0) {
                        conn = &connections[i];
                        break;
                    }
                }
                // 여전히 없으면 새로 할당
                if (conn == NULL) {
                    conn = allocate_connection();
                }
            }
            
            if (conn != NULL) {
                conn->conn_id = param->connect.conn_id;
                memcpy(conn->remote_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
                conn->is_connected = true;
                conn->is_connecting = false;
                active_connections++;
                
                char mac_str[18];
                device_collector_mac_to_string(conn->remote_bda, mac_str);
                ESP_LOGI(TAG, "연결 추가: conn_id=%d, MAC=%s, 총 연결=%d", 
                        conn->conn_id, mac_str, active_connections);
            }
            
            // 하위 호환성: 전역 변수 업데이트 (마지막 연결)
            is_connected = true;
            is_connecting = false;
            g_conn_id = param->connect.conn_id;
            memcpy(g_remote_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            
            // 디바이스 수집기에 객체 생성
            esp_err_t create_ret = device_collector_create_device(param->connect.remote_bda);
            if (create_ret == ESP_OK) {
                char mac_str[18];
                device_collector_mac_to_string(param->connect.remote_bda, mac_str);
                ESP_LOGI(TAG, "디바이스 수집기 객체 생성 완료: %s", mac_str);
            } else {
                ESP_LOGE(TAG, "디바이스 수집기 객체 생성 실패: %s", esp_err_to_name(create_ret));
            }

            
            
            // MTU 요청 (연결 직후)
            uint16_t conn_id_to_use = (conn != NULL) ? conn->conn_id : g_conn_id;
            esp_err_t mtu_ret = esp_ble_gattc_send_mtu_req(gattc_if, conn_id_to_use);
            ESP_LOGI(TAG, "MTU req (conn_id=%d): %s", conn_id_to_use, esp_err_to_name(mtu_ret));
            
            // MTU 설정은 생략 (필요 시 IDF 버전에 맞는 API로 교체)
            
            // 전체 서비스 탐색 시작
            ESP_LOGI(TAG, "전체 서비스 탐색 시작 (conn_id=%d)...", conn_id_to_use);
            esp_ble_gattc_search_service(s_gattc_if, conn_id_to_use, NULL);
            break;
            
        case ESP_GATTC_SEARCH_RES_EVT: {
            uint16_t s = param->search_res.start_handle;
            uint16_t e = param->search_res.end_handle;
            uint16_t conn_id = param->search_res.conn_id;
            
            // 연결 컨텍스트 찾기
            device_connection_t* conn = find_connection_by_conn_id(conn_id);
            if (conn == NULL) {
                ESP_LOGW(TAG, "conn_id %d에 대한 연결 컨텍스트를 찾을 수 없습니다", conn_id);
                break;
            }
            
            ESP_LOGI(TAG, "서비스 발견 (conn_id=%d): start=%d end=%d", conn_id, s, e);

            // 이 서비스 범위에서 NUS TX(Notify) 특성 UUID로 직접 탐색하여 구독 시도
            uint16_t count = 1;
            esp_gattc_char_elem_t ch = {0};
            esp_err_t rc = esp_ble_gattc_get_char_by_uuid(s_gattc_if, conn_id, s, e, NUS_TX_CHAR_UUID, &ch, &count);
            if (rc == ESP_OK && count > 0) {
                // 연결 컨텍스트에 저장
                conn->service_start = s;
                conn->service_end = e;
                conn->tx_char_handle = ch.char_handle;
                
                // 하위 호환성: 전역 변수도 업데이트
                g_service_start = s;
                g_service_end = e;
                g_tx_char_handle = ch.char_handle;
                ESP_LOGI(TAG, "TX 특성 발견 및 구독 시도 (conn_id=%d) - handle:%d (service %d~%d)", 
                        conn_id, conn->tx_char_handle, s, e);
                esp_ble_gattc_register_for_notify(s_gattc_if, conn->remote_bda, conn->tx_char_handle);

                // CCCD 설정
                esp_bt_uuid_t cccd_uuid = { .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG } };
                esp_gattc_descr_elem_t d = {0};
                uint16_t dcount = 1;
                if (esp_ble_gattc_get_descr_by_char_handle(s_gattc_if, conn_id, conn->tx_char_handle, cccd_uuid, &d, &dcount) == ESP_OK && dcount > 0) {
                    conn->cccd_handle = d.handle;
                    g_cccd_handle = d.handle; // 하위 호환성
                    uint8_t notify_en[2] = {0x01, 0x00};
                    esp_err_t wr = esp_ble_gattc_write_char_descr(s_gattc_if, conn_id, conn->cccd_handle,
                                          sizeof(notify_en), notify_en,
                                          ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
                    ESP_LOGI(TAG, "CCCD write(conn_id=%d, handle:%u): %s", conn_id, (unsigned)conn->cccd_handle, esp_err_to_name(wr));
                    // CCCD 읽기 검증
                    esp_err_t rr = esp_ble_gattc_read_char_descr(s_gattc_if, conn_id, conn->cccd_handle, ESP_GATT_AUTH_REQ_NONE);
                    ESP_LOGI(TAG, "CCCD read req (conn_id=%d): %s", conn_id, esp_err_to_name(rr));
                }

                // RX(Write) 특성도 보조로 획득
                esp_gattc_char_elem_t chrx = {0};
                count = 1;
                if (esp_ble_gattc_get_char_by_uuid(s_gattc_if, conn_id, s, e, NUS_RX_CHAR_UUID, &chrx, &count) == ESP_OK && count > 0) {
                    conn->rx_char_handle = chrx.char_handle;
                    conn->rx_char_properties = chrx.properties;  // properties 저장
                    g_rx_char_handle = chrx.char_handle; // 하위 호환성
                    ESP_LOGI(TAG, "NUS RX(Write) 특성 핸들 (conn_id=%d): %d, properties: 0x%02x", 
                            conn_id, conn->rx_char_handle, chrx.properties);
                    
                    // Write 가능 여부 확인
                    bool can_write = (chrx.properties & ESP_GATT_CHAR_PROP_BIT_WRITE) != 0;
                    bool can_write_no_rsp = (chrx.properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) != 0;
                    ESP_LOGI(TAG, "RX Characteristic Properties - WRITE: %s, WRITE_NO_RSP: %s",
                            can_write ? "YES" : "NO", can_write_no_rsp ? "YES" : "NO");
                } else {
                    ESP_LOGW(TAG, "NUS RX characteristic을 찾지 못했습니다 (conn_id=%d)", conn_id);
                }
            }
            break;
        }
            
        case ESP_GATTC_SEARCH_CMPL_EVT: {
            uint16_t conn_id = param->search_cmpl.conn_id;
            ESP_LOGI(TAG, "서비스 탐색 완료 (conn_id=%d) - status: %d", conn_id, param->search_cmpl.status);
            
            // 연결 컨텍스트 찾기
            device_connection_t* conn = find_connection_by_conn_id(conn_id);
            if (conn == NULL || param->search_cmpl.status != ESP_GATT_OK || 
                conn->service_start == 0 || conn->service_end == 0) {
                ESP_LOGW(TAG, "NUS 서비스 범위를 찾지 못했습니다 (conn_id=%d)", conn_id);
                break;
            }

            // TX(Notify) 특성 찾기 (NUS 기준)
            uint16_t count = 0;
            esp_gattc_char_elem_t char_elem = {0};
            count = 1;
            esp_err_t retc = esp_ble_gattc_get_char_by_uuid(s_gattc_if, conn_id,
                                conn->service_start, conn->service_end,
                                NUS_TX_CHAR_UUID,
                                &char_elem, &count);
            if (retc == ESP_OK && count > 0) {
                conn->tx_char_handle = char_elem.char_handle;
                g_tx_char_handle = char_elem.char_handle; // 하위 호환성
                ESP_LOGI(TAG, "NUS TX(Notify) 특성 핸들 (conn_id=%d): %d", conn_id, conn->tx_char_handle);

                // Notify 등록
                esp_err_t rn = esp_ble_gattc_register_for_notify(s_gattc_if, conn->remote_bda, conn->tx_char_handle);
                ESP_LOGI(TAG, "register_for_notify (conn_id=%d): %s", conn_id, esp_err_to_name(rn));

                // CCCD 찾기 (0x2902)
                esp_bt_uuid_t cccd_uuid = { .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG } };
                esp_gattc_descr_elem_t descr_elem = {0};
                count = 1;
                esp_err_t retd = esp_ble_gattc_get_descr_by_char_handle(s_gattc_if, conn_id,
                                    conn->tx_char_handle, cccd_uuid, &descr_elem, &count);
                if (retd == ESP_OK && count > 0) {
                    conn->cccd_handle = descr_elem.handle;
                    g_cccd_handle = descr_elem.handle; // 하위 호환성
                    ESP_LOGI(TAG, "CCCD 핸들 (conn_id=%d): %d", conn_id, conn->cccd_handle);
                    uint8_t notify_en[2] = {0x01, 0x00};
                    esp_err_t rw = esp_ble_gattc_write_char_descr(s_gattc_if, conn_id, conn->cccd_handle,
                                        sizeof(notify_en), notify_en,
                                        ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
                    ESP_LOGI(TAG, "CCCD write (conn_id=%d): %s", conn_id, esp_err_to_name(rw));
                } else {
                    ESP_LOGW(TAG, "CCCD를 찾지 못했습니다 (conn_id=%d)", conn_id);
                }
                
                // RX(Write) 특성 핸들 저장 (필요 시 사용)
                // 중요: bt_nus의 RX characteristic은 반드시 찾아야 함
                esp_gattc_char_elem_t char_rx = {0};
                count = 1;
                esp_err_t rett = esp_ble_gattc_get_char_by_uuid(s_gattc_if, conn_id,
                                    conn->service_start, conn->service_end,
                                    NUS_RX_CHAR_UUID,
                                    &char_rx, &count);
                if (rett == ESP_OK && count > 0) {
                    conn->rx_char_handle = char_rx.char_handle;
                    conn->rx_char_properties = char_rx.properties;  // properties 저장
                    g_rx_char_handle = char_rx.char_handle; // 하위 호환성
                    ESP_LOGI(TAG, "✓ NUS RX(Write) 특성 핸들 발견 (conn_id=%d): handle=%d, properties=0x%02x", 
                            conn_id, conn->rx_char_handle, char_rx.properties);
                    
                    // Write 가능 여부 확인
                    bool can_write = (char_rx.properties & ESP_GATT_CHAR_PROP_BIT_WRITE) != 0;
                    bool can_write_no_rsp = (char_rx.properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) != 0;
                    ESP_LOGI(TAG, "  -> WRITE: %s, WRITE_NO_RSP: %s",
                            can_write ? "YES" : "NO", can_write_no_rsp ? "YES" : "NO");
                    
                    if (!can_write && !can_write_no_rsp) {
                        ESP_LOGE(TAG, "  -> 경고: RX characteristic이 write를 지원하지 않습니다!");
                    }
                } else {
                    ESP_LOGE(TAG, "✗ NUS RX(Write) 특성을 찾지 못했습니다 (conn_id=%d, ret=%s, count=%d)", 
                            conn_id, esp_err_to_name(rett), count);
                    ESP_LOGE(TAG, "  -> 서비스 범위: start=%d, end=%d", conn->service_start, conn->service_end);
                    ESP_LOGE(TAG, "  -> 데이터 전송이 불가능합니다!");
                }
            } else {
                ESP_LOGW(TAG, "NUS TX(Notify) 특성을 찾지 못했습니다 (conn_id=%d)", conn_id);
            }

            // 추가: 서비스 내 모든 특성 중 Notify/Indicate 속성을 가진 특성 자동 구독
            {
                uint16_t ccount = 0;
                if (esp_ble_gattc_get_attr_count(s_gattc_if, conn_id,
                        ESP_GATT_DB_CHARACTERISTIC, conn->service_start, conn->service_end, 0, &ccount) == ESP_GATT_OK && ccount > 0) {
                    esp_gattc_char_elem_t *chars = (esp_gattc_char_elem_t*)malloc(sizeof(esp_gattc_char_elem_t) * ccount);
                    if (chars) {
                        if (esp_ble_gattc_get_all_char(s_gattc_if, conn_id, conn->service_start, conn->service_end, chars, &ccount, 0) == ESP_OK) {
                            for (int i = 0; i < ccount; i++) {
                                uint8_t prop = chars[i].properties;
                                bool wants_notify = (prop & ESP_GATT_CHAR_PROP_BIT_NOTIFY) != 0;
                                bool wants_indicate = (prop & ESP_GATT_CHAR_PROP_BIT_INDICATE) != 0;
                                if (wants_notify || wants_indicate) {
                                    uint16_t h = chars[i].char_handle;
                                    ESP_LOGI(TAG, "자동 구독 대상 특성 (conn_id=%d) handle:%u prop:0x%02x", conn_id, (unsigned)h, prop);
                                    esp_ble_gattc_register_for_notify(s_gattc_if, conn->remote_bda, h);
                                    // CCCD 찾기 및 설정
                                    esp_bt_uuid_t cccd_uuid = { .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG } };
                                    esp_gattc_descr_elem_t descr = {0};
                                    uint16_t dcount = 1;
                                    if (esp_ble_gattc_get_descr_by_char_handle(s_gattc_if, conn_id, h, cccd_uuid, &descr, &dcount) == ESP_OK && dcount > 0) {
                                        uint8_t val[2] = { (uint8_t)(wants_notify ? 0x01 : 0x02), 0x00 }; // notify=0x0001, indicate=0x0002
                                        esp_err_t w = esp_ble_gattc_write_char_descr(s_gattc_if, conn_id, descr.handle,
                                                                sizeof(val), val,
                                                                ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
                                        ESP_LOGI(TAG, "CCCD set (conn_id=%d, handle:%u) -> %s", conn_id, (unsigned)descr.handle, esp_err_to_name(w));
                                    }
                                }
                            }
                        }
                        free(chars);
                    }
                }
            }
            break;
        }
            
        case ESP_GATTC_DISCONNECT_EVT:
            {
                uint16_t conn_id = param->disconnect.conn_id;
                ESP_LOGI(TAG, "디바이스 연결 해제됨 - conn_id: %d", conn_id);
                
                // 연결 컨텍스트 찾기
                device_connection_t* conn = find_connection_by_conn_id(conn_id);
                if (conn != NULL) {
                    // 디바이스 수집기에서 객체 제거
                    esp_err_t remove_ret = device_collector_remove_device(conn->remote_bda);
                    if (remove_ret == ESP_OK) {
                        char mac_str[18];
                        device_collector_mac_to_string(conn->remote_bda, mac_str);
                        ESP_LOGI(TAG, "디바이스 수집기 객체 제거 완료: %s", mac_str);
                    } else {
                        ESP_LOGW(TAG, "디바이스 수집기 객체 제거 실패: %s", esp_err_to_name(remove_ret));
                    }
                    
                    // 연결 제거
                    remove_connection(conn_id);
                }
                
                // 하위 호환성: 마지막 연결이면 전역 변수 업데이트
                if (active_connections == 0) {
                    is_connected = false;
                }
            }
            break;
            
        case ESP_GATTC_REG_FOR_NOTIFY_EVT:
            ESP_LOGI(TAG, "Notification 등록 완료! - status: %d", param->reg_for_notify.status);
            break;
        
        case ESP_GATTC_WRITE_DESCR_EVT: {
            ESP_LOGI(TAG, "WRITE_DESCR_EVT - status:%d handle:%d", param->write.status, param->write.handle);
            // CCCD 활성화 완료 로그만 남기고 추가 트리거 전송은 수행하지 않음
            break;
        }
            
        case ESP_GATTC_NOTIFY_EVT:
            {
                uint16_t conn_id = param->notify.conn_id;
                // ble 데이터 수신
                // ESP_LOGI(TAG, "디바이스로부터 데이터 수신 (conn_id=%d) - 길이: %d", conn_id, param->notify.value_len);

                // 데이터 내용 출력 (문자열로 가정)
                if (param->notify.value_len > 0) {
                    char data_str[256];
                    size_t copy_len = param->notify.value_len < sizeof(data_str) - 1 ? param->notify.value_len : sizeof(data_str) - 1;
                    memcpy(data_str, param->notify.value, copy_len);
                    data_str[copy_len] = '\0';
                    // ESP_LOGI(TAG, "수신 데이터: %s", data_str);
                }

                // 연결 컨텍스트 찾기
                device_connection_t* conn = find_connection_by_conn_id(conn_id);
                if (conn != NULL) {
                    // 받은 데이터 저장 (하위 호환성을 위해 전역 변수에도 저장)
                    if (param->notify.value_len > 0 && param->notify.value_len <= sizeof(received_data)) {
                        memcpy(received_data, param->notify.value, param->notify.value_len);
                        received_data_len = param->notify.value_len;
                        
                        // 전역 변수 업데이트 (마지막 수신 디바이스)
                        g_conn_id = conn_id;
                        memcpy(g_remote_bda, conn->remote_bda, sizeof(esp_bd_addr_t));
                    }
                    
                    // MAC 주소를 파라미터로 전달하는 새로운 콜백 (다중 연결 지원)
                    // 기존 콜백은 전역 변수 사용하므로 호환성 유지
                    ble_device_on_notify_with_mac(conn->remote_bda, param->notify.value, param->notify.value_len);
                } else {
                    ESP_LOGW(TAG, "conn_id %d에 대한 연결 컨텍스트를 찾을 수 없습니다", conn_id);
                    // 기존 방식으로 폴백
                    ble_device_on_notify(param->notify.value, param->notify.value_len);
                }
            }
            break;
            
        case ESP_GATTC_READ_CHAR_EVT:
            ESP_LOGI(TAG, "Characteristic 읽기 완료 - value_len: %d",
                    param->read.value_len);
            break;
            
        case ESP_GATTC_WRITE_CHAR_EVT:
            {
                uint16_t conn_id = param->write.conn_id;
                uint16_t handle = param->write.handle;
                esp_gatt_status_t status = param->write.status;
                
                device_connection_t* conn = find_connection_by_conn_id(conn_id);
                char mac_str[18] = "unknown";
                if (conn != NULL) {
                    device_collector_mac_to_string(conn->remote_bda, mac_str);
                }
                
                if (status == ESP_GATT_OK) {
                    ESP_LOGI(TAG, "✓ Characteristic 쓰기 완료 - status: %d (성공), handle: %d, conn_id: %d, MAC: %s",
                            status, handle, conn_id, mac_str);
                    // WRITE_RSP를 사용한 경우에만 서버 수신 확인 가능
                    // NO_RSP를 사용한 경우 이 이벤트가 발생하지 않을 수 있음 (정상 동작)
                    ESP_LOGI(TAG, "  -> Write 이벤트 수신 (WRITE_RSP 사용 시 서버 수신 확인됨)");
                } else {
                    ESP_LOGE(TAG, "✗ Characteristic 쓰기 실패 - status: %d (0x%02x), handle: %d, conn_id: %d, MAC: %s",
                            status, status, handle, conn_id, mac_str);
                    ESP_LOGE(TAG, "  -> 서버에서 데이터를 거부했거나 처리하지 못했습니다");
                }
            }
            break;
            
        default:
            ESP_LOGD(TAG, "처리되지 않은 GATTC 이벤트: %d", event);
            break;
    }
}

// GAP 이벤트 핸들러
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_SCAN_RESULT_EVT: {
            if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
                uint8_t *adv_data = param->scan_rst.ble_adv;
                uint8_t adv_data_len = param->scan_rst.adv_data_len;
                uint8_t *adv_name = NULL;
                uint8_t adv_name_len = 0;
                
                // 디바이스 이름 추출 (올바른 AD 구조 파싱)
                int i = 0;
                while (i < adv_data_len) {
                    uint8_t length = adv_data[i];
                    if (length == 0 || i + length >= adv_data_len) {
                        break;  // 잘못된 데이터
                    }

                    uint8_t ad_type = adv_data[i + 1];
                    if (ad_type == 0x09 || ad_type == 0x08) {  // Complete or Shortened Local Name
                        adv_name = &adv_data[i + 2];
                        adv_name_len = length - 1;  // Length에서 AD Type 1바이트 제외
                        break;
                    }

                    i += (length + 1);  // 다음 AD 구조로 이동 (Length 필드 + 데이터)
                }
                
                if (adv_name != NULL && adv_name_len > 0) {
                    char device_name[32] = {0};
                    strncpy(device_name, (char*)adv_name,
                           (adv_name_len < sizeof(device_name) - 1) ? adv_name_len : sizeof(device_name) - 1);

                    // 이름 필터링 확인
                    bool name_match = true;
                    if (strlen(target_device_name) > 0) {
                        name_match = (strcmp(device_name, target_device_name) == 0);
                    }

                    // MAC 주소 필터링 확인
                    bool mac_match = false;

                    // MAC 주소 배열이 있는 경우 (target_mac_list_count > 0)
                    if (target_mac_list_count > 0) {
                        // 배열의 MAC 주소 중 하나라도 일치하면 true
                        for (int i = 0; i < target_mac_list_count; i++) {
                            if (memcmp(param->scan_rst.bda, target_mac_list[i], 6) == 0) {
                                mac_match = true;
                                break;
                            }
                        }
                    } else if (target_mac_set) {
                        // 단일 MAC 주소 필터링 (하위 호환성)
                        mac_match = (memcmp(param->scan_rst.bda, target_mac_address, 6) == 0);
                    } else {
                        // MAC 필터링 없음
                        mac_match = true;
                    }

                    // 이름 일치 OR MAC 주소 일치 (mac_count > 0인 경우)
                    // 이름 일치 AND MAC 주소 일치 (단일 MAC 또는 필터 없음)
                    bool device_match = false;
                    if (target_mac_list_count > 0) {
                        // MAC 배열이 있으면: 이름 일치 OR MAC 배열 중 하나 일치
                        device_match = (name_match || mac_match);
                    } else {
                        // MAC 배열 없으면: 이름 일치 AND (MAC 일치 또는 MAC 필터 없음)
                        device_match = (name_match && mac_match);
                    }

                    if (device_match) {
                        ESP_LOGI(TAG, "타겟 디바이스 발견: %s", device_name);
                        
                        // 중복 확인
                        bool already_scanned = false;
                        for (int i = 0; i < scanned_device_count; i++) {
                            if (memcmp(scanned_devices[i].mac_address, param->scan_rst.bda, 6) == 0) {
                                already_scanned = true;
                                break;
                            }
                        }
                        
                        if (!already_scanned && scanned_device_count < MAX_SCAN_DEVICES) {
                            // 리스트에 추가
                            memcpy(scanned_devices[scanned_device_count].mac_address, param->scan_rst.bda, 6);
                            scanned_devices[scanned_device_count].is_used = false;
                            scanned_device_count++;
                            
                            ESP_LOGI(TAG, "스캔 리스트에 추가 [%d/%d]: %02x:%02x:%02x:%02x:%02x:%02x",
                                    scanned_device_count, MAX_SCAN_DEVICES,
                                    param->scan_rst.bda[0], param->scan_rst.bda[1], param->scan_rst.bda[2],
                                    param->scan_rst.bda[3], param->scan_rst.bda[4], param->scan_rst.bda[5]);
                        }
                        
                        // 단일 연결 호환성 (첫 번째 발견된 디바이스)
                        if (!device_found) {
                            memcpy(found_device_mac, param->scan_rst.bda, 6);
                            device_found = true;
                            
                            // 단일 연결 모드에서는 첫 발견 시 스캔 중지 (다중 연결 모드에서는 계속 스캔)
                            // 다중 연결 함수에서 제어함
                        }
                    }
                }
            }
            break;
        }
        
        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            ESP_LOGI(TAG, "스캔 완료");
            break;
            
        default:
            break;
    }
}

// 연결 관리 함수 구현
static device_connection_t* find_connection_by_conn_id(uint16_t conn_id) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections[i].is_connected && connections[i].conn_id == conn_id) {
            return &connections[i];
        }
    }
    return NULL;
}

static device_connection_t* find_connection_by_mac(const uint8_t* mac_address) {
    if (mac_address == NULL) return NULL;
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections[i].is_connected && 
            memcmp(connections[i].remote_bda, mac_address, 6) == 0) {
            return &connections[i];
        }
    }
    return NULL;
}

static device_connection_t* allocate_connection(void) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (!connections[i].is_connected && !connections[i].is_connecting) {
            memset(&connections[i], 0, sizeof(device_connection_t));
            return &connections[i];
        }
    }
    return NULL;
}

static void remove_connection(uint16_t conn_id) {
    device_connection_t* conn = find_connection_by_conn_id(conn_id);
    if (conn != NULL) {
        conn->is_connected = false;
        conn->is_connecting = false;
        active_connections--;
        ESP_LOGI(TAG, "연결 제거: conn_id=%d, 남은 연결: %d", conn_id, active_connections);
    }
}

// BLE 초기화 (클라이언트 모드)
esp_err_t ble_device_init(void) {
    if (ble_device_is_init) {
        ESP_LOGW(TAG, "BLE가 이미 초기화되어 있습니다");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "BLE 클라이언트 초기화 시작");
    
    // 연결 배열 초기화
    memset(connections, 0, sizeof(connections));
    active_connections = 0;
    scanned_device_count = 0;
    memset(scanned_devices, 0, sizeof(scanned_devices));
    
    // BT 컨트롤러 초기화
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "BT 컨트롤러 초기화 실패: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "BT 컨트롤러 활성화 실패: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Bluedroid 초기화
    ret = esp_bluedroid_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Bluedroid 초기화 실패: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Bluedroid 활성화 실패: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // GATT 클라이언트 콜백 등록
    ret = esp_ble_gattc_register_callback(gattc_event_handler);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "GATT 클라이언트 콜백 등록 실패: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // GATT 클라이언트 앱 등록
    ret = esp_ble_gattc_app_register(0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GATT 클라이언트 앱 등록 실패: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // GAP 콜백 등록
    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GAP 콜백 등록 실패: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ble_device_is_init = true;
    ESP_LOGI(TAG, "BLE 클라이언트 초기화 완료");
    
    return ESP_OK;
}

// BLE 초기화 여부 확인
bool ble_device_is_init_func(void) {
    return ble_device_is_init;
}

// BLE 스캔 함수
bool ble_device_scan(const char* device_name, const char* mac_address) {
    if (!ble_device_is_init) {
        ESP_LOGE(TAG, "BLE가 초기화되지 않았습니다");
        return false;
    }
    
    if (is_scanning) {
        ESP_LOGW(TAG, "이미 스캔 중입니다");
        return false;
    }
    
    // 파라미터 초기화
    memset(target_device_name, 0, sizeof(target_device_name));
    memset(target_mac_address, 0, sizeof(target_mac_address));
    target_mac_set = false;
    device_found = false;
    memset(found_device_mac, 0, sizeof(found_device_mac));
    
    // 디바이스 이름 설정
    if (device_name != NULL && strlen(device_name) > 0) {
        strncpy(target_device_name, device_name, sizeof(target_device_name) - 1);
        ESP_LOGI(TAG, "스캔 대상 디바이스 이름: %s", target_device_name);
    }
    
    // MAC 주소 설정
    if (mac_address != NULL && strlen(mac_address) > 0) {
        if (parse_mac_address(mac_address, target_mac_address)) {
            target_mac_set = true;
            ESP_LOGI(TAG, "스캔 대상 MAC 주소: %02x:%02x:%02x:%02x:%02x:%02x",
                    target_mac_address[0], target_mac_address[1], target_mac_address[2],
                    target_mac_address[3], target_mac_address[4], target_mac_address[5]);
        } else {
            ESP_LOGE(TAG, "잘못된 MAC 주소 형식: %s", mac_address);
            return false;
        }
    }
    
    // 스캔 파라미터 설정
    // own_addr_type을 PUBLIC으로 변경 (RANDOM은 random address 설정이 필요함)
    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,
        .scan_window = 0x30,
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE
    };
    
    esp_err_t ret = esp_ble_gap_set_scan_params(&scan_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "스캔 파라미터 설정 실패: %s", esp_err_to_name(ret));
        return false;
    }
    
    // 스캔 시작
    is_scanning = true;
    ESP_LOGI(TAG, "=== BLE 스캔 시작 (최대 10초) ===");
    
    ret = esp_ble_gap_start_scanning(10); // 10초 스캔
    if (ret != ESP_OK) {
        is_scanning = false;
        ESP_LOGE(TAG, "스캔 시작 실패: %s", esp_err_to_name(ret));
        return false;
    }
    
    // 스캔 완료 대기 (최대 11초)
    int timeout = 11; // 10초 + 여유 1초
    while (is_scanning && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        timeout--;
    }
    
    is_scanning = false;
    
    if (device_found) {
        ESP_LOGI(TAG, "=== 디바이스 발견 (스캔 성공) ===");
        
        // 이전 연결이 있으면 먼저 해제
        if (is_connected || is_connecting) {
            ESP_LOGI(TAG, "이전 연결/연결 시도가 있습니다. 리셋합니다.");
            is_connected = false;
            is_connecting = false;
            vTaskDelay(pdMS_TO_TICKS(500));  // 상태 리셋 시간
        }
        
        // GATT 인터페이스가 준비되지 않았으면 대기
        int retry_count = 0;
        while (s_gattc_if == ESP_GATT_IF_NONE && retry_count < 10) {
            ESP_LOGW(TAG, "GATT 인터페이스 대기 중... (%d/10)", retry_count);
            vTaskDelay(pdMS_TO_TICKS(500));
            retry_count++;
        }
        
        if (s_gattc_if == ESP_GATT_IF_NONE) {
            ESP_LOGE(TAG, "GATT 인터페이스 준비 실패");
            return false;
        }
        
        // 추가 대기 시간 - 스캔 완료 후 연결 안정화
        vTaskDelay(pdMS_TO_TICKS(500));
        
        // 발견한 디바이스에 연결 시도
        ESP_LOGI(TAG, "연결 시도 시작... (gattc_if=%d)", s_gattc_if);
        ESP_LOGI(TAG, "연결 대상 MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                 found_device_mac[0], found_device_mac[1], found_device_mac[2],
                 found_device_mac[3], found_device_mac[4], found_device_mac[5]);
        
        is_connecting = true;
        is_connected = false; // 초기화
        
        // RANDOM으로 먼저 시도
        esp_err_t ret = esp_ble_gattc_open(s_gattc_if, found_device_mac, BLE_ADDR_TYPE_RANDOM, true);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "RANDOM 타입으로 연결 실패: %s", esp_err_to_name(ret));
            // PUBLIC으로 재시도
            ESP_LOGI(TAG, "PUBLIC 타입으로 재시도...");
            ret = esp_ble_gattc_open(s_gattc_if, found_device_mac, BLE_ADDR_TYPE_PUBLIC, true);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "PUBLIC 타입으로도 연결 실패: %s", esp_err_to_name(ret));
                is_connecting = false;
                return false;
            }
        }
        
        ESP_LOGI(TAG, "연결 시도 성공, 연결 완료 대기 중...");
        
        // 연결 완료 대기 (최대 15초로 증가)
        timeout = 15;
        while (is_connecting && timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            timeout--;
        }
        
        if (is_connected) {
            ESP_LOGI(TAG, "=== 디바이스 연결 성공 ===");
            return true;
        } else {
            ESP_LOGW(TAG, "=== 디바이스 연결 시간 초과 ===");
            return false;
        }
    } else {
        ESP_LOGI(TAG, "=== 디바이스를 찾지 못했습니다 (스캔 실패) ===");
        return false;
    }
}

// 다중 연결용 스캔 함수 - 모든 타겟 디바이스를 리스트로 수집
int ble_device_scan_multiple(const char* device_name, char mac_addresses[][18], int mac_count) {
    if (!ble_device_is_init) {
        ESP_LOGE(TAG, "BLE가 초기화되지 않았습니다");
        return 0;
    }

    if (is_scanning) {
        ESP_LOGW(TAG, "이미 스캔 중입니다");
        return 0;
    }

    // 파라미터 초기화
    memset(target_device_name, 0, sizeof(target_device_name));
    memset(target_mac_address, 0, sizeof(target_mac_address));
    target_mac_set = false;
    memset(target_mac_list, 0, sizeof(target_mac_list));
    target_mac_list_count = 0;
    scanned_device_count = 0;
    memset(scanned_devices, 0, sizeof(scanned_devices));
    device_found = false; // 다중 연결에서는 스캔 중지 안함

    // 디바이스 이름 설정
    if (device_name != NULL && strlen(device_name) > 0) {
        strncpy(target_device_name, device_name, sizeof(target_device_name) - 1);
        ESP_LOGI(TAG, "다중 스캔 대상 디바이스 이름: %s", target_device_name);
    }

    // MAC 주소 배열 설정
    if (mac_addresses != NULL && mac_count > 0) {
        int valid_count = 0;
        for (int i = 0; i < mac_count && i < MAX_TARGET_MACS; i++) {
            if (strlen(mac_addresses[i]) > 0) {
                if (parse_mac_address(mac_addresses[i], target_mac_list[valid_count])) {
                    ESP_LOGI(TAG, "스캔 대상 MAC[%d]: %02x:%02x:%02x:%02x:%02x:%02x",
                            valid_count,
                            target_mac_list[valid_count][0], target_mac_list[valid_count][1],
                            target_mac_list[valid_count][2], target_mac_list[valid_count][3],
                            target_mac_list[valid_count][4], target_mac_list[valid_count][5]);
                    valid_count++;
                } else {
                    ESP_LOGW(TAG, "잘못된 MAC 주소 형식 (무시): %s", mac_addresses[i]);
                }
            }
        }
        target_mac_list_count = valid_count;

        if (target_mac_list_count > 0) {
            ESP_LOGI(TAG, "총 %d개의 MAC 주소를 스캔 대상으로 설정", target_mac_list_count);
            ESP_LOGI(TAG, "스캔 모드: 이름(\"%s\") OR MAC 주소 배열 중 하나",
                    strlen(target_device_name) > 0 ? target_device_name : "모두");
        }
    }
    
    // 스캔 파라미터 설정
    // own_addr_type을 PUBLIC으로 변경 (RANDOM은 random address 설정이 필요함)
    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,
        .scan_window = 0x30,
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE
    };
    
    esp_err_t ret = esp_ble_gap_set_scan_params(&scan_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "스캔 파라미터 설정 실패: %s", esp_err_to_name(ret));
        return 0;
    }
    
    // 스캔 시작
    is_scanning = true;
    ESP_LOGI(TAG, "=== 다중 스캔 시작 (최대 10초, 모든 디바이스 수집) ===");
    
    ret = esp_ble_gap_start_scanning(10); // 10초 스캔
    if (ret != ESP_OK) {
        is_scanning = false;
        ESP_LOGE(TAG, "스캔 시작 실패: %s", esp_err_to_name(ret));
        return 0;
    }
    
    // 스캔 완료 대기 (최대 11초)
    int timeout = 11; // 10초 + 여유 1초
    while (is_scanning && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        timeout--;
    }
    
    is_scanning = false;
    
    ESP_LOGI(TAG, "=== 스캔 완료: 발견된 디바이스 %d개 ===", scanned_device_count);
    
    return scanned_device_count;
}

// 다중 연결 함수 - 스캔된 모든 디바이스에 연결 시도
int ble_device_connect_multiple(void) {
    if (!ble_device_is_init) {
        ESP_LOGE(TAG, "BLE가 초기화되지 않았습니다");
        return 0;
    }
    
    if (scanned_device_count == 0) {
        ESP_LOGW(TAG, "스캔된 디바이스가 없습니다");
        return 0;
    }
    
    // GATT 인터페이스가 준비되지 않았으면 대기
    int retry_count = 0;
    while (s_gattc_if == ESP_GATT_IF_NONE && retry_count < 10) {
        ESP_LOGW(TAG, "GATT 인터페이스 대기 중... (%d/10)", retry_count);
        vTaskDelay(pdMS_TO_TICKS(500));
        retry_count++;
    }
    
    if (s_gattc_if == ESP_GATT_IF_NONE) {
        ESP_LOGE(TAG, "GATT 인터페이스 준비 실패");
        return 0;
    }
    
    ESP_LOGI(TAG, "=== 다중 연결 시작: %d개 디바이스에 연결 시도 ===", scanned_device_count);
    
    int connected_count = 0;
    
    // 각 디바이스에 순차적으로 연결 시도
    for (int i = 0; i < scanned_device_count; i++) {
        if (scanned_devices[i].is_used) {
            continue; // 이미 연결된 디바이스는 스킵
        }
        
        uint8_t* mac = scanned_devices[i].mac_address;
        char mac_str[18];
        device_collector_mac_to_string(mac, mac_str);
        
        ESP_LOGI(TAG, "[%d/%d] 연결 시도: %s", i + 1, scanned_device_count, mac_str);
        
        // 연결 컨텍스트 할당
        device_connection_t* conn = allocate_connection();
        if (conn == NULL) {
            ESP_LOGE(TAG, "[%d/%d] 연결 슬롯 부족 (최대 %d개)", i + 1, scanned_device_count, MAX_CONNECTIONS);
            continue;
        }
        
        conn->is_connecting = true;
        memcpy(conn->remote_bda, mac, 6);
        
        // RANDOM으로 먼저 시도
        esp_err_t ret = esp_ble_gattc_open(s_gattc_if, mac, BLE_ADDR_TYPE_RANDOM, true);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "[%d/%d] RANDOM 타입으로 연결 실패: %s", i + 1, scanned_device_count, esp_err_to_name(ret));
            // PUBLIC으로 재시도
            ret = esp_ble_gattc_open(s_gattc_if, mac, BLE_ADDR_TYPE_PUBLIC, true);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "[%d/%d] PUBLIC 타입으로도 연결 실패: %s", i + 1, scanned_device_count, esp_err_to_name(ret));
                conn->is_connecting = false;
                continue;
            }
        }
        
        // 연결 완료 대기 (최대 15초)
        int timeout = 15;
        while (conn->is_connecting && !conn->is_connected && timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            timeout--;
        }
        
        if (conn->is_connected) {
            scanned_devices[i].is_used = true;
            connected_count++;
            ESP_LOGI(TAG, "[%d/%d] 연결 성공! 총 연결: %d개", i + 1, scanned_device_count, connected_count);
        } else {
            ESP_LOGW(TAG, "[%d/%d] 연결 시간 초과", i + 1, scanned_device_count);
            conn->is_connecting = false;
        }
        
        // 다음 연결 전 짧은 대기 (연결 안정화)
        if (i < scanned_device_count - 1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    
    ESP_LOGI(TAG, "=== 다중 연결 완료: %d개 디바이스 연결 성공 (전체 %d개 중) ===", 
             connected_count, scanned_device_count);
    
    return connected_count;
}

// BLE 연결 해제 및 초기화 해제
void ble_device_disconnect(void) {
    ESP_LOGI(TAG, "=== BLE 연결 해제 시작 ===");
    
    // 연결된 디바이스가 있다면 연결 해제
    if (device_found) {
        ESP_LOGI(TAG, "연결된 디바이스 연결 해제");
        device_found = false;
        memset(found_device_mac, 0, sizeof(found_device_mac));
    }
    
    // 스캔 중이면 중지
    if (is_scanning) {
        ESP_LOGI(TAG, "스캔 중지");
        esp_ble_gap_stop_scanning();
        is_scanning = false;
    }
    
    // 초기화 해제
    if (ble_device_is_init) {
        ESP_LOGI(TAG, "BLE 초기화 해제");
        
        // BT 스택 해제 (간단하게)
        esp_bluedroid_disable();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        
        ble_device_is_init = false;
    }
    
    // 타겟 정보 초기화
    memset(target_device_name, 0, sizeof(target_device_name));
    memset(target_mac_address, 0, sizeof(target_mac_address));
    target_mac_set = false;
    
    ESP_LOGI(TAG, "=== BLE 연결 해제 완료 ===");
}

// 특정 MAC 주소를 가진 BLE 디바이스 연결 해제
int ble_device_disconnect_by_mac(const char* mac_address) {
    if (mac_address == NULL || strlen(mac_address) == 0) {
        ESP_LOGE(TAG, "유효하지 않은 MAC 주소");
        return -1;
    }

    // MAC 주소 문자열을 바이트 배열로 변환
    uint8_t mac_bytes[6];
    if (sscanf(mac_address, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &mac_bytes[0], &mac_bytes[1], &mac_bytes[2],
               &mac_bytes[3], &mac_bytes[4], &mac_bytes[5]) != 6) {
        ESP_LOGE(TAG, "MAC 주소 파싱 실패: %s", mac_address);
        return -1;
    }

    // 연결된 디바이스 찾기
    device_connection_t* conn = find_connection_by_mac(mac_bytes);
    if (conn == NULL) {
        ESP_LOGW(TAG, "연결된 디바이스를 찾을 수 없습니다: %s", mac_address);
        return -1;
    }

    // GATT 연결 해제
    esp_err_t ret = esp_ble_gattc_close(s_gattc_if, conn->conn_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GATT 연결 해제 실패 (MAC: %s, conn_id: %d): %s",
                 mac_address, conn->conn_id, esp_err_to_name(ret));
        return -1;
    }

    ESP_LOGI(TAG, "디바이스 연결 해제 요청 성공 (MAC: %s, conn_id: %d)", mac_address, conn->conn_id);

    // 연결 해제는 ESP_GATTC_DISCONNECT_EVT 이벤트에서 처리됨
    return 0;
}

// BLE로 받은 데이터 출력
void ble_device_print(void) {
    if (received_data_len == 0) {
        ESP_LOGI(TAG, "받은 데이터 없음");
        return;
    }
    
    ESP_LOGI(TAG, "=== BLE 데이터 수신 ===");
    ESP_LOGI(TAG, "데이터 길이: %d bytes", received_data_len);
    
    // 16진수로 출력
    char hex_string[512] = {0};
    int offset = 0;
    for (uint16_t i = 0; i < received_data_len && offset < 510; i++) {
        offset += sprintf(&hex_string[offset], "%02x ", received_data[i]);
    }
    ESP_LOGI(TAG, "데이터 (hex): %s", hex_string);
    
    // ASCII 문자열로 출력 시도
    bool is_ascii = true;
    for (uint16_t i = 0; i < received_data_len; i++) {
        if (received_data[i] < 32 || received_data[i] > 126) {
            is_ascii = false;
            break;
        }
    }
    
    if (is_ascii) {
        char ascii_string[256] = {0};
        int copy_len = (received_data_len < sizeof(ascii_string) - 1) ? received_data_len : sizeof(ascii_string) - 1;
        memcpy(ascii_string, received_data, copy_len);
        ascii_string[copy_len] = '\0';
        ESP_LOGI(TAG, "데이터 (ASCII): %s", ascii_string);
    }
    
    ESP_LOGI(TAG, "========================");
}

// 연결된 디바이스 정보 출력
void ble_info_print(void) {
    ESP_LOGI(TAG, "=== BLE 연결 정보 ===");
    ESP_LOGI(TAG, "초기화됨: %s", ble_device_is_init ? "true" : "false");
    ESP_LOGI(TAG, "스캔중: %s, 연결중: %s, 연결됨: %s",
             is_scanning ? "true" : "false",
             is_connecting ? "true" : "false",
             is_connected ? "true" : "false");

    // 발견된 디바이스 MAC (스캔 결과)
    if (device_found) {
        ESP_LOGI(TAG, "발견된 MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                 found_device_mac[0], found_device_mac[1], found_device_mac[2],
                 found_device_mac[3], found_device_mac[4], found_device_mac[5]);
    } else {
        ESP_LOGI(TAG, "발견된 MAC: (없음)");
    }

    // 실제 연결된 원격 BDA
    if (is_connected) {
        ESP_LOGI(TAG, "연결된 BDA: %02x:%02x:%02x:%02x:%02x:%02x",
                 g_remote_bda[0], g_remote_bda[1], g_remote_bda[2],
                 g_remote_bda[3], g_remote_bda[4], g_remote_bda[5]);
        ESP_LOGI(TAG, "conn_id: %u", (unsigned)g_conn_id);
        // MTU: IDF v5.1에서는 이벤트로 확인 권장 (직접 조회 호출 제거)
        ESP_LOGI(TAG, "서비스 범위: start=%u end=%u", (unsigned)g_service_start, (unsigned)g_service_end);
        ESP_LOGI(TAG, "핸들 - RX:%u TX:%u CCCD:%u",
                 (unsigned)g_rx_char_handle, (unsigned)g_tx_char_handle, (unsigned)g_cccd_handle);

        // 서비스 범위 내 특성/디스크립터 상세 나열 (NUS 또는 현재 선택된 서비스)
        if (g_service_start != 0 && g_service_end != 0) {
            // Characteristic 나열
            uint16_t ccount = 0;
            if (esp_ble_gattc_get_attr_count(s_gattc_if, g_conn_id,
                    ESP_GATT_DB_CHARACTERISTIC, g_service_start, g_service_end, 0, &ccount) == ESP_GATT_OK && ccount > 0) {
                esp_gattc_char_elem_t *chars = (esp_gattc_char_elem_t*)malloc(sizeof(esp_gattc_char_elem_t) * ccount);
                if (chars) {
                    if (esp_ble_gattc_get_all_char(s_gattc_if, g_conn_id,
                            g_service_start, g_service_end, chars, &ccount, 0) == ESP_OK) {
                        ESP_LOGI(TAG, "특성 %d개:", ccount);
                        for (int i = 0; i < ccount; i++) {
                            esp_bt_uuid_t cuid = chars[i].uuid;
                            // UUID 출력
                            if (cuid.len == ESP_UUID_LEN_16) {
                                ESP_LOGI(TAG, "  - char handle:%u uuid16:0x%04x prop:0x%02x",
                                         (unsigned)chars[i].char_handle, cuid.uuid.uuid16, chars[i].properties);
                            } else if (cuid.len == ESP_UUID_LEN_32) {
                                ESP_LOGI(TAG, "  - char handle:%u uuid32:0x%08" PRIx32 " prop:0x%02x",
                                         (unsigned)chars[i].char_handle, (uint32_t)cuid.uuid.uuid32, chars[i].properties);
                            } else if (cuid.len == ESP_UUID_LEN_128) {
                                uint8_t *u = cuid.uuid.uuid128;
                                ESP_LOGI(TAG, "  - char handle:%u uuid128:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x prop:0x%02x",
                                         (unsigned)chars[i].char_handle,
                                         u[15],u[14],u[13],u[12], u[11],u[10], u[9],u[8], u[7],u[6], u[5],u[4],u[3],u[2],u[1],u[0],
                                         chars[i].properties);
                            }

                            // 해당 특성의 디스크립터 나열
                            uint16_t dcount = 0;
                            if (esp_ble_gattc_get_attr_count(s_gattc_if, g_conn_id,
                                    ESP_GATT_DB_DESCRIPTOR, g_service_start, g_service_end, chars[i].char_handle, &dcount) == ESP_GATT_OK && dcount > 0) {
                                esp_gattc_descr_elem_t *descrs = (esp_gattc_descr_elem_t*)malloc(sizeof(esp_gattc_descr_elem_t) * dcount);
                                if (descrs) {
                                    if (esp_ble_gattc_get_all_descr(s_gattc_if, g_conn_id, chars[i].char_handle,
                                            descrs, &dcount, 0) == ESP_OK) {
                                        for (int j = 0; j < dcount; j++) {
                                            if (descrs[j].uuid.len == ESP_UUID_LEN_16) {
                                                ESP_LOGI(TAG, "     · descr handle:%u uuid16:0x%04x",
                                                         (unsigned)descrs[j].handle, descrs[j].uuid.uuid.uuid16);
                                            } else if (descrs[j].uuid.len == ESP_UUID_LEN_32) {
                                                ESP_LOGI(TAG, "     · descr handle:%u uuid32:0x%08" PRIx32,
                                                         (unsigned)descrs[j].handle, (uint32_t)descrs[j].uuid.uuid.uuid32);
                                            } else if (descrs[j].uuid.len == ESP_UUID_LEN_128) {
                                                uint8_t *du = descrs[j].uuid.uuid.uuid128;
                                                ESP_LOGI(TAG, "     · descr handle:%u uuid128:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                                                         (unsigned)descrs[j].handle,
                                                         du[15],du[14],du[13],du[12], du[11],du[10], du[9],du[8], du[7],du[6], du[5],du[4],du[3],du[2],du[1],du[0]);
                                            }
                                        }
                                    }
                                    free(descrs);
                                }
                            }
                        }
                    }
                    free(chars);
                }
            } else {
                ESP_LOGI(TAG, "특성 없음 (서비스 범위 내)");
            }
        }
    } else {
        ESP_LOGI(TAG, "현재 연결 없음");
    }

    // 최근 수신 데이터도 요약 출력
    if (received_data_len > 0) {
        ESP_LOGI(TAG, "%02x:%02x:%02x:%02x:%02x:%02x,%.*s",
                 g_remote_bda[0], g_remote_bda[1], g_remote_bda[2],
                 g_remote_bda[3], g_remote_bda[4], g_remote_bda[5],
                 (int)received_data_len, (const char*)received_data);
    } else {
        ESP_LOGI(TAG, "최근 수신 데이터 없음");
    }

    ESP_LOGI(TAG, "=====================");
}

// 현재 활성 BLE 연결 수 조회
int ble_device_get_active_connections(void) {
    return active_connections;
}

// BLE 클라이언트로 데이터 전송 (모든 연결된 디바이스에 전송)
int ble_device_send_data(const uint8_t* data, uint16_t length) {
    if (data == NULL || length == 0) {
        ESP_LOGE(TAG, "전송할 데이터가 NULL이거나 길이가 0입니다");
        return 0;
    }
    
    if (s_gattc_if == ESP_GATT_IF_NONE) {
        ESP_LOGW(TAG, "GATTC 인터페이스가 초기화되지 않았습니다");
        return 0;
    }
    
    int success_count = 0;
    
    // 모든 연결된 디바이스에 데이터 전송
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections[i].is_connected) {
            char mac_str[18];
            device_collector_mac_to_string(connections[i].remote_bda, mac_str);
            
            // Serial Bluetooth Terminal과 동일하게 매번 UUID를 통해 RX characteristic 찾기
            // 서비스 범위 확인
            if (connections[i].service_start == 0 || connections[i].service_end == 0) {
                ESP_LOGE(TAG, "서비스 범위가 없습니다 (MAC: %s). 서비스 탐색이 완료되지 않았습니다.", mac_str);
                continue;
            }
            
            // UUID를 통해 RX characteristic 찾기 (매번 찾기 - Serial Bluetooth Terminal 방식)
            // 중요: 서비스 탐색이 완료되지 않았거나 서비스 범위가 없으면 실패
            if (connections[i].service_start == 0 || connections[i].service_end == 0) {
                ESP_LOGE(TAG, "✗ 서비스 범위가 없습니다 (MAC: %s). 서비스 탐색이 완료되지 않았습니다.", mac_str);
                continue;
            }
            
            esp_gattc_char_elem_t char_rx = {0};
            uint16_t count = 1;
            esp_err_t rett = esp_ble_gattc_get_char_by_uuid(s_gattc_if, connections[i].conn_id,
                                connections[i].service_start, connections[i].service_end,
                                NUS_RX_CHAR_UUID,
                                &char_rx, &count);
            
            if (rett != ESP_OK || count == 0) {
                ESP_LOGE(TAG, "✗ UUID로 RX characteristic을 찾지 못했습니다 (MAC: %s, ret=%s, count=%d)", 
                        mac_str, esp_err_to_name(rett), count);
                ESP_LOGE(TAG, "  -> 서비스 범위: start=%d, end=%d", 
                        connections[i].service_start, connections[i].service_end);
                ESP_LOGE(TAG, "  -> UUID: 6e400002-b5a3-f393-e0a9-e50e24dcca9e");
                continue;
            }
            
            uint16_t rx_char_handle = char_rx.char_handle;
            uint8_t properties = char_rx.properties;
            
            // 연결 컨텍스트 업데이트 (참고용)
            connections[i].rx_char_handle = rx_char_handle;
            connections[i].rx_char_properties = properties;
            
            ESP_LOGD(TAG, "✓ UUID로 RX characteristic 발견 (MAC: %s): handle=%d, properties=0x%02x", 
                    mac_str, rx_char_handle, properties);
            
            // Handle 검증: 0이면 안됨
            if (rx_char_handle == 0) {
                ESP_LOGE(TAG, "✗ RX characteristic handle이 0입니다 (MAC: %s) - 잘못된 handle", mac_str);
                continue;
            }
            
            // Handle이 서비스 범위 내에 있는지 검증
            if (rx_char_handle < connections[i].service_start || rx_char_handle > connections[i].service_end) {
                ESP_LOGE(TAG, "✗ RX characteristic handle이 서비스 범위를 벗어났습니다 (MAC: %s)", mac_str);
                ESP_LOGE(TAG, "  -> handle=%d, 서비스 범위: %d~%d", 
                        rx_char_handle, connections[i].service_start, connections[i].service_end);
                continue;
            }
            
            // RX characteristic의 properties에 따라 적절한 write type 선택
            // Serial Bluetooth Terminal과 React Native는 WRITE_NO_RSP를 사용하므로 동일하게 적용
            esp_gatt_write_type_t write_type;
            
            // Serial Bluetooth Terminal과 동일하게 WRITE_NO_RSP를 강제로 사용
            // properties에 WRITE_NO_RSP가 있으면 NO_RSP 사용, 없으면 RSP 사용
            if ((properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) != 0) {
                write_type = ESP_GATT_WRITE_TYPE_NO_RSP;  // NO_RSP 우선 (Serial Bluetooth Terminal 방식)
                ESP_LOGD(TAG, "WRITE_NO_RSP 사용 (MAC: %s)", mac_str);
            } else if ((properties & ESP_GATT_CHAR_PROP_BIT_WRITE) != 0) {
                write_type = ESP_GATT_WRITE_TYPE_RSP;  // Response 기다림
                ESP_LOGD(TAG, "WRITE (with response) 사용 (MAC: %s)", mac_str);
            } else {
                ESP_LOGE(TAG, "RX characteristic이 write를 지원하지 않습니다! (MAC: %s, properties: 0x%02x)", 
                        mac_str, properties);
                continue;
            }
            
            // 로그 출력 최소화 (블로킹 방지)
            ESP_LOGD(TAG, "데이터 전송 시도 (MAC: %s, handle: %d, write_type: %s, 길이: %d)", 
                    mac_str, rx_char_handle,
                    (write_type == ESP_GATT_WRITE_TYPE_RSP) ? "RSP" : "NO_RSP", length);
            ESP_LOGD(TAG, "전송 데이터: %.*s", length, (const char*)data);
            
            // Serial Bluetooth Terminal과 동일하게 NO_RSP로 전송
            // NO_RSP는 응답을 기다리지 않으므로 빠르지만, 실제 수신 여부는 확인 불가
            esp_err_t ret = esp_ble_gattc_write_char(
                s_gattc_if,
                connections[i].conn_id,
                rx_char_handle,  // UUID로 확인한 최신 handle 사용
                length,
                (uint8_t*)data,
                write_type,
                ESP_GATT_AUTH_REQ_NONE
            );
            
            // NO_RSP는 응답을 기다리지 않으므로, ESP_GATTC_WRITE_CHAR_EVT가 발생하지 않을 수 있음
            // 하지만 Serial Bluetooth Terminal도 NO_RSP를 사용하므로 이것이 정상 동작
            
            if (ret == ESP_OK) {
                success_count++;
                ESP_LOGI(TAG, "✓ 데이터 전송 요청 성공 (MAC: %s, conn_id: %d, handle: %d, write_type: %s)", 
                        mac_str, connections[i].conn_id, rx_char_handle,
                        (write_type == ESP_GATT_WRITE_TYPE_RSP) ? "RSP" : "NO_RSP");
                // Write 완료 이벤트(ESP_GATTC_WRITE_CHAR_EVT)에서 실제 전송 성공 여부 확인됨
            } else {
                ESP_LOGE(TAG, "✗ 데이터 전송 요청 실패 (MAC: %s, conn_id: %d, handle: %d): %s", 
                        mac_str, connections[i].conn_id, rx_char_handle, esp_err_to_name(ret));
            }
        }
    }
    
    return success_count;
}

// 연결된 BLE 디바이스의 MAC 주소를 가져오는 함수
int ble_device_get_connected_mac_addresses(char mac_addresses[][18], int max_count)
{
    if (mac_addresses == NULL || max_count <= 0) {
        ESP_LOGE(TAG, "잘못된 파라미터");
        return 0;
    }
    
    int count = 0;
    for (int i = 0; i < MAX_CONNECTIONS && count < max_count; i++) {
        if (connections[i].is_connected) {
            device_collector_mac_to_string(connections[i].remote_bda, mac_addresses[count]);
            count++;
        }
    }
    
    return count;
}

// BLE 클라이언트로 문자열 전송 (모든 연결된 디바이스에 전송)
// React Native 방식과 동일하게 원본 텍스트 그대로 전송 (개행 문자 자동 추가 안 함)
int ble_device_send_message(const char* message) {
    if (message == NULL) {
        ESP_LOGE(TAG, "전송할 메시지가 NULL입니다");
        return 0;
    }

    size_t len = strlen(message);
    if (len == 0) {
        ESP_LOGE(TAG, "전송할 메시지가 비어있습니다");
        return 0;
    }

    // React Native와 동일하게 원본 텍스트 그대로 전송 (개행 문자 자동 추가 안 함)
    // React Native: Array.from(text, (char) => char.charCodeAt(0)) - 원본 그대로 바이트 배열로 변환
    // 개행 문자가 필요하면 호출자가 명시적으로 포함해야 함
    int result = ble_device_send_data((const uint8_t*)message, (uint16_t)len);
    return result;
}

// 특정 MAC 주소를 가진 BLE 디바이스에 문자열 전송
int ble_target_device_send_message(const char* mac_address, const char* message) {
    if (mac_address == NULL) {
        ESP_LOGE(TAG, "MAC 주소가 NULL입니다");
        return 0;
    }

    if (message == NULL) {
        ESP_LOGE(TAG, "전송할 메시지가 NULL입니다");
        return 0;
    }

    size_t len = strlen(message);
    if (len == 0) {
        ESP_LOGE(TAG, "전송할 메시지가 비어있습니다");
        return 0;
    }

    // MAC 주소 문자열을 바이트 배열로 변환
    uint8_t target_mac[6] = {0};
    if (!parse_mac_address(mac_address, target_mac)) {
        ESP_LOGE(TAG, "잘못된 MAC 주소 형식: %s", mac_address);
        return 0;
    }

    // 연결된 디바이스 중에서 해당 MAC 주소를 가진 디바이스 찾기
    device_connection_t* conn = find_connection_by_mac(target_mac);
    if (conn == NULL) {
        ESP_LOGW(TAG, "해당 MAC 주소를 가진 연결된 디바이스를 찾을 수 없습니다: %s", mac_address);
        return 0;
    }

    if (!conn->is_connected) {
        ESP_LOGW(TAG, "디바이스가 연결되어 있지 않습니다: %s", mac_address);
        return 0;
    }

    if (s_gattc_if == ESP_GATT_IF_NONE) {
        ESP_LOGW(TAG, "GATTC 인터페이스가 초기화되지 않았습니다");
        return 0;
    }

    char mac_str[18];
    device_collector_mac_to_string(conn->remote_bda, mac_str);

    // 서비스 범위 확인
    if (conn->service_start == 0 || conn->service_end == 0) {
        ESP_LOGE(TAG, "서비스 범위가 없습니다 (MAC: %s). 서비스 탐색이 완료되지 않았습니다.", mac_str);
        return 0;
    }

    // UUID를 통해 RX characteristic 찾기
    esp_gattc_char_elem_t char_rx = {0};
    uint16_t count = 1;
    esp_err_t rett = esp_ble_gattc_get_char_by_uuid(s_gattc_if, conn->conn_id,
                        conn->service_start, conn->service_end,
                        NUS_RX_CHAR_UUID,
                        &char_rx, &count);

    if (rett != ESP_OK || count == 0) {
        ESP_LOGE(TAG, "UUID로 RX characteristic을 찾지 못했습니다 (MAC: %s, ret=%s, count=%d)",
                mac_str, esp_err_to_name(rett), count);
        return 0;
    }

    uint16_t rx_char_handle = char_rx.char_handle;
    uint8_t properties = char_rx.properties;

    // Handle 검증
    if (rx_char_handle == 0) {
        ESP_LOGE(TAG, "RX characteristic handle이 0입니다 (MAC: %s)", mac_str);
        return 0;
    }

    if (rx_char_handle < conn->service_start || rx_char_handle > conn->service_end) {
        ESP_LOGE(TAG, "RX characteristic handle이 서비스 범위를 벗어났습니다 (MAC: %s)", mac_str);
        return 0;
    }

    // Write type 선택
    esp_gatt_write_type_t write_type;
    if ((properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) != 0) {
        write_type = ESP_GATT_WRITE_TYPE_NO_RSP;
    } else if ((properties & ESP_GATT_CHAR_PROP_BIT_WRITE) != 0) {
        write_type = ESP_GATT_WRITE_TYPE_RSP;
    } else {
        ESP_LOGE(TAG, "RX characteristic이 write를 지원하지 않습니다 (MAC: %s, properties: 0x%02x)",
                mac_str, properties);
        return 0;
    }

    ESP_LOGI(TAG, "타겟 디바이스로 데이터 전송 시도 (MAC: %s, 길이: %d)", mac_str, len);
    ESP_LOGI(TAG, "전송 데이터: %s", message);

    // 데이터 전송
    esp_err_t ret = esp_ble_gattc_write_char(
        s_gattc_if,
        conn->conn_id,
        rx_char_handle,
        (uint16_t)len,
        (uint8_t*)message,
        write_type,
        ESP_GATT_AUTH_REQ_NONE
    );

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "타겟 디바이스로 데이터 전송 성공 (MAC: %s)", mac_str);
        return 1;
    } else {
        ESP_LOGE(TAG, "타겟 디바이스로 데이터 전송 실패 (MAC: %s): %s", mac_str, esp_err_to_name(ret));
        return 0;
    }
}
