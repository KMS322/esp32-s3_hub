#include "ble_client.h"
#include "esp_log.h"
#include "string.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"

static const char *TAG = "BLE_CLIENT";

// 연결된 디바이스 목록 (최대 5개)
#define MAX_CONNECTED_DEVICES 5
static nrf5340_device_t connected_devices[MAX_CONNECTED_DEVICES];
static int connected_device_count = 0;

// 스캔 중인 디바이스 정보
static char target_device_name[32] = {0};
static uint8_t target_mac_addresses[6 * MAX_CONNECTED_DEVICES] = {0};
static int target_mac_count = 0;
static bool is_scanning = false;

// 발견한 디바이스 연결 정보
static esp_bd_addr_t found_device_mac = {0};
static bool device_found = false;
static bool connection_in_progress = false;

// BLE 클라이언트 초기화 상태
static bool ble_client_initialized = false;

// GATT 클라이언트 인터페이스 및 연결 정보
static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;
static esp_gatt_srvc_id_t g_service_id = {0};
static uint16_t g_conn_id = 0;
static esp_bd_addr_t g_device_addr = {0};

// BLE 클라이언트용 GAP 이벤트 핸들러 (이름 변경)
void ble_client_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_SCAN_RESULT_EVT: {
            esp_ble_gap_cb_param_t *scan_result = (esp_ble_gap_cb_param_t *)param;
            
            if (scan_result->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
                // 스캔 결과 처리
                uint8_t *adv_data = scan_result->scan_rst.ble_adv;
                uint8_t adv_data_len = scan_result->scan_rst.adv_data_len;
                uint8_t *adv_name = NULL;
                uint8_t adv_name_len = 0;
                
                // 디바이스 이름 추출
                for (int i = 0; i < adv_data_len; i++) {
                    if (adv_data[i] == 0x09 || adv_data[i] == 0x08) { // Complete Local Name 또는 Short Local Name
                        adv_name = &adv_data[i + 2];
                        adv_name_len = adv_data[i + 1];
                        break;
                    }
                }
                
                if (adv_name != NULL && adv_name_len > 0) {
                    char device_name[32] = {0};
                    strncpy(device_name, (char*)adv_name, 
                           (adv_name_len < sizeof(device_name) - 1) ? adv_name_len : sizeof(device_name) - 1);
                    
                    // 디바이스 이름이 타겟과 일치하는지 확인
                    if (strcmp(device_name, target_device_name) == 0) {
                        ESP_LOGI(TAG, "타겟 디바이스 발견: %s", device_name);
                        
                        // MAC 주소 확인
                        uint8_t *mac_addr = scan_result->scan_rst.bda;
                        bool mac_match = false;
                        
                        if (target_mac_count > 0) {
                            for (int i = 0; i < target_mac_count; i++) {
                                if (memcmp(mac_addr, &target_mac_addresses[i*6], 6) == 0) {
                                    mac_match = true;
                                    ESP_LOGI(TAG, "MAC 주소 일치: %02x:%02x:%02x:%02x:%02x:%02x", 
                                             mac_addr[0], mac_addr[1], mac_addr[2], 
                                             mac_addr[3], mac_addr[4], mac_addr[5]);
                                    break;
                                }
                            }
                        } else {
                            mac_match = true; // MAC 필터링 없음
                        }
                        
                        if (mac_match) {
                            ESP_LOGI(TAG, "타겟 디바이스와 일치합니다!");
                            ESP_LOGI(TAG, "발견된 MAC 주소: %02x:%02x:%02x:%02x:%02x:%02x", 
                              mac_addr[0], mac_addr[1], mac_addr[2], 
                              mac_addr[3], mac_addr[4], mac_addr[5]);
                            
                            // 발견한 디바이스 정보 저장
                            memcpy(found_device_mac, mac_addr, 6);
                            device_found = true;
                            
                            // 스캔 중지
                            esp_ble_gap_stop_scanning();
                            is_scanning = false;
                            ESP_LOGI(TAG, "스캔 중지 요청, 스캔 완료 이벤트 대기 중...");
                        } else {
                            ESP_LOGW(TAG, "디바이스 이름은 일치하지만 MAC 주소가 다릅니다");
                        }
                    }
                }
            }
            break;
        }
        
        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            ESP_LOGI(TAG, "스캔이 완료되었습니다 - device_found=%d", device_found);
            is_scanning = false;  // 스캔 플래그 리셋
            
            // 발견한 디바이스에 연결 시도
            if (device_found) {
                ESP_LOGI(TAG, "device_found=true, 연결 시도 진행");
                device_found = false;
                
                // GATT 클라이언트 인터페이스 확인
                if (s_gattc_if == ESP_GATT_IF_NONE) {
                    ESP_LOGE(TAG, "GATT 클라이언트 인터페이스가 준비되지 않았습니다. 재스캔...");
                    esp_ble_gap_start_scanning(30);
                    break;
                }
                
                // 잠시 대기 (스캔 완전 종료 확인)
                vTaskDelay(pdMS_TO_TICKS(200));
                
                // 디바이스에 연결 시도
                ESP_LOGI(TAG, "========== 연결 시도 시작 ==========");
                ESP_LOGI(TAG, "gattc_if=%d", s_gattc_if);
                ESP_LOGI(TAG, "연결할 MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                        found_device_mac[0], found_device_mac[1], found_device_mac[2],
                        found_device_mac[3], found_device_mac[4], found_device_mac[5]);
                
                connection_in_progress = true;  // 연결 중 플래그 설정
                // RANDOM으로 먼저 시도
                esp_err_t ret = esp_ble_gattc_open(s_gattc_if, found_device_mac, BLE_ADDR_TYPE_RANDOM, true);
                ESP_LOGI(TAG, "esp_ble_gattc_open(RANDOM) 결과: %s (0x%x)", esp_err_to_name(ret), ret);
                
                if (ret != ESP_OK) {
                    ESP_LOGI(TAG, "PUBLIC 타입으로 재시도...");
                    ret = esp_ble_gattc_open(s_gattc_if, found_device_mac, BLE_ADDR_TYPE_PUBLIC, true);
                    ESP_LOGI(TAG, "esp_ble_gattc_open(PUBLIC) 결과: %s (0x%x)", esp_err_to_name(ret), ret);
                }
                
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "연결 시도 실패! 상태 확인 필요");
                    ESP_LOGE(TAG, "10초 후 재스캔 시도...");
                    connection_in_progress = false;  // 연결 실패시 리셋
                    // 10초 대기 후 재시도를 위해 스캔 재시작
                    vTaskDelay(pdMS_TO_TICKS(10000));
                    is_scanning = true;  // 재스캔 플래그 설정
                    esp_ble_gap_start_scanning(30);
                } else {
                    ESP_LOGI(TAG, "연결 요청 성공! 연결 이벤트 대기 중...");
                }
                ESP_LOGI(TAG, "=====================================");
            }
            break;
            
        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            ESP_LOGI(TAG, "연결 파라미터 업데이트 이벤트");
            break;
            
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            ESP_LOGI(TAG, "스캔 파라미터 설정 완료");
            break;
            
        default:
            break;
    }
}

static void ble_client_gattc_event_handler(esp_gattc_cb_event_t event,
  esp_gatt_if_t gattc_if,
  esp_ble_gattc_cb_param_t *param) {
    switch (event) {
    case ESP_GATTC_REG_EVT:
        ESP_LOGI(TAG, "========== GATT 클라이언트 등록 완료 ==========");
        ESP_LOGI(TAG, "app_id=%d, gattc_if=%d", param->reg.app_id, gattc_if);
        s_gattc_if = gattc_if;
        ESP_LOGI(TAG, "GATT 인터페이스 설정됨: s_gattc_if=%d", s_gattc_if);
        break;

    case ESP_GATTC_OPEN_EVT:
        ESP_LOGI(TAG, "========== GATT 연결 시도 결과 ==========");
        ESP_LOGI(TAG, "conn_id=%d, status=%d (0x%02x), remote_bda=%02x:%02x:%02x:%02x:%02x:%02x",
                param->open.conn_id, param->open.status, param->open.status,
                param->open.remote_bda[0], param->open.remote_bda[1], param->open.remote_bda[2],
                param->open.remote_bda[3], param->open.remote_bda[4], param->open.remote_bda[5]);
        
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "연결 시도 실패: status=%d (0x%02x)", param->open.status, param->open.status);
            connection_in_progress = false;  // 연결 중 플래그 리셋
            is_scanning = false;
            
            // 연결 실패시 재스캔
            ESP_LOGI(TAG, "3초 후 재스캔 시도...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_ble_gap_start_scanning(30);
            is_scanning = true;
        } else {
            ESP_LOGI(TAG, "연결 시도 성공! 연결 완료 이벤트 대기 중...");
            // ESP_GATTC_CONNECT_EVT가 다음에 발생할 것
        }
        ESP_LOGI(TAG, "=====================================");
        break;

    case ESP_GATTC_CONNECT_EVT:
        ESP_LOGI(TAG, "디바이스 연결됨, conn_id=%d", param->connect.conn_id);
        g_conn_id = param->connect.conn_id;
        memcpy(g_device_addr, param->connect.remote_bda, 6);
        
        connection_in_progress = false;  // 연결 완료
        
        // 연결된 디바이스 정보 저장
        for (int i = 0; i < MAX_CONNECTED_DEVICES; i++) {
            if (!connected_devices[i].is_connected) {
                memcpy(connected_devices[i].mac_address, g_device_addr, 6);
                strncpy(connected_devices[i].device_name, target_device_name, sizeof(connected_devices[i].device_name) - 1);
                connected_devices[i].conn_id = g_conn_id;
                connected_devices[i].gattc_if = gattc_if;
                connected_devices[i].is_connected = true;
                connected_device_count++;
                ESP_LOGI(TAG, "연결된 디바이스 추가 완료 (총 %d개)", connected_device_count);
                break;
            }
        }
        
        // 서비스 탐색 시작
        ESP_LOGI(TAG, "서비스 탐색 시작...");
        esp_ble_gattc_search_service(gattc_if, param->connect.conn_id, NULL);
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGE(TAG, "디바이스 연결 해제됨 - reason=%d (0x%x)", param->disconnect.reason, param->disconnect.reason);
        ESP_LOGE(TAG, "연결 상태: conn_id=%d, gattc_if=%d", param->disconnect.conn_id, gattc_if);
        
        connection_in_progress = false;  // 연결 해제시 리셋
        
        // 연결 해제된 디바이스 정보 제거
        for (int i = 0; i < MAX_CONNECTED_DEVICES; i++) {
            if (memcmp(connected_devices[i].mac_address, param->disconnect.remote_bda, 6) == 0) {
                connected_devices[i].is_connected = false;
                memset(&connected_devices[i], 0, sizeof(nrf5340_device_t));
                connected_device_count--;
                ESP_LOGI(TAG, "연결 해제된 디바이스 제거 완료 (남은 개수: %d)", connected_device_count);
                break;
            }
        }
        
        // 서비스 정보 초기화
        memset(&g_service_id, 0, sizeof(g_service_id));
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        ESP_LOGI(TAG, "서비스 발견");
        // 첫 번째 서비스 저장
        if (g_service_id.id.uuid.len == 0) {
            memcpy(&g_service_id.id, &param->search_res.srvc_id, sizeof(esp_gatt_id_t));
            g_service_id.is_primary = true; // 기본값 설정
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
        ESP_LOGI(TAG, "서비스 탐색 완료");
        // TODO: ESP-IDF v5.1에서 Characteristic 검색 API 변경됨
        // 나중에 필요시 올바른 API로 구현 필요
        ESP_LOGI(TAG, "Characteristic 검색은 추후 구현 예정");
        break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
        ESP_LOGI(TAG, "Notification 등록 완료!");
        break;

    case ESP_GATTC_NOTIFY_EVT:
        ESP_LOGI(TAG, "디바이스로부터 데이터 수신 - Length: %d", param->notify.value_len);
        
        // 데이터 수신 리스너 호출
        if (param->notify.value_len > 0) {
            ble_client_receive_devices(param->notify.value, param->notify.value_len);
        }
        break;

    default:
        break;
  }
}

// BLE 클라이언트 초기화
esp_err_t ble_client_init(void) {
  esp_err_t ret;

  ESP_LOGI(TAG, "========== BLE 클라이언트 초기화 시작 ==========");

  // 이미 초기화되어 있으면 스킵
  if (ble_client_initialized) {
    ESP_LOGI(TAG, "BLE 클라이언트가 이미 초기화되어 있습니다");
    return ESP_OK;
  }

  // BT 스택이 이미 초기화되어 있는지 확인
  // ble_gatt.c의 ble_init()이나 다른 곳에서 이미 초기화했을 수 있음
  
  // BT 컨트롤러 초기화 시도
  ESP_LOGI(TAG, "BT 컨트롤러 확인 중...");
  esp_log_level_set("BLE_INIT", ESP_LOG_NONE);
  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  ret = esp_bt_controller_init(&bt_cfg);
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "BT 컨트롤러 초기화 실패: %s", esp_err_to_name(ret));
    return ret;
  }
  
  ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "BT 컨트롤러 활성화 실패: %s", esp_err_to_name(ret));
    return ret;
  }
  
  // Bluedroid 초기화 및 활성화
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
  
  ESP_LOGI(TAG, "BT 스택 준비 완료");
  
  ESP_LOGI(TAG, "GATT 클라이언트 콜백 등록 시도...");
  
  // 3. GATT 클라이언트 콜백 등록
  ret = esp_ble_gattc_register_callback(ble_client_gattc_event_handler);
  if (ret == ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "GATT 서버가 이미 등록되어 있습니다. 다시 시도...");
    return ret;
  }
  if (ret) {
    ESP_LOGE(TAG, "GATT 클라이언트 콜백 등록 실패: %s", esp_err_to_name(ret));
    return ret;
  }

  // 4. GATT 클라이언트 앱 등록
  ret = esp_ble_gattc_app_register(0);
  if (ret) {
    ESP_LOGE(TAG, "GATT 클라이언트 앱 등록 실패: %s", esp_err_to_name(ret));
    return ret;
  }

  // 연결된 디바이스 목록 초기화
  memset(connected_devices, 0, sizeof(connected_devices));
  connected_device_count = 0;

  ble_client_initialized = true;
  ESP_LOGI(TAG, "BLE 클라이언트 초기화 완료");
  return ESP_OK;
}

// BLE 클라이언트 초기화 여부 확인
bool ble_client_is_initialized(void) {
    return ble_client_initialized;
}

// BLE 스캔 함수
esp_err_t ble_client_scan_devices(const char* device_name, const uint8_t* mac_addresses, int mac_count) {
    if (device_name == NULL) {
        ESP_LOGE(TAG, "디바이스 이름이 NULL입니다");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!ble_client_is_initialized()) {
        ESP_LOGE(TAG, "BLE 클라이언트가 초기화되지 않았습니다");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 타겟 정보 저장
    strncpy(target_device_name, device_name, sizeof(target_device_name) - 1);
    target_device_name[sizeof(target_device_name) - 1] = '\0';
    
    if (mac_addresses != NULL && mac_count > 0) {
        memcpy(target_mac_addresses, mac_addresses, mac_count * 6);
        target_mac_count = mac_count;
        ESP_LOGI(TAG, "MAC 주소 개수: %d", mac_count);
    } else {
        target_mac_count = 0;
        ESP_LOGI(TAG, "MAC 주소 필터링 없이 스캔합니다");
    }
    
    ESP_LOGI(TAG, "디바이스 스캔 시작 - 이름: %s", target_device_name);
    
    // GAP 이벤트 핸들러 등록 (함수 이름 변경)
    esp_err_t ret = esp_ble_gap_register_callback(ble_client_gap_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GAP 콜백 등록 실패: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 스캔 파라미터 설정
    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_RANDOM,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,
        .scan_window = 0x30,
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE
    };
    
    ret = esp_ble_gap_set_scan_params(&scan_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "스캔 파라미터 설정 실패: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 이미 스캔 중이면 스킵
    if (is_scanning) {
        ESP_LOGW(TAG, "이미 스캔이 진행 중입니다. 스캔을 시작하지 않습니다.");
        return ESP_OK;
    }
    
    // 스캔 시작
    is_scanning = true;
    ret = esp_ble_gap_start_scanning(30); // 30초 스캔
    if (ret != ESP_OK) {
        is_scanning = false;
        ESP_LOGE(TAG, "스캔 시작 실패: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "스캔이 시작되었습니다");
    return ESP_OK;
}

// 연결된 디바이스 목록 가져오기
int ble_client_get_connected_devices(nrf5340_device_t* devices, int max_devices) {
    if (devices == NULL || max_devices <= 0) {
        return 0;
    }
    
    int count = (connected_device_count < max_devices) ? connected_device_count : max_devices;
    memcpy(devices, connected_devices, count * sizeof(nrf5340_device_t));
    
    return count;
}

// 특정 디바이스 연결 상태 확인
bool ble_client_is_device_connected(const uint8_t* mac_address) {
    if (mac_address == NULL) {
        return false;
    }
    
    for (int i = 0; i < connected_device_count; i++) {
        if (memcmp(connected_devices[i].mac_address, mac_address, 6) == 0) {
            return connected_devices[i].is_connected;
        }
    }
    
    return false;
}

// 연결된 디바이스가 있는지 확인
bool ble_client_has_connected_devices(void) {
    return connected_device_count > 0;
}

// BLE 클라이언트 정리
void ble_client_cleanup(void) {
    ESP_LOGI(TAG, "BLE 클라이언트 정리");
    
    // 모든 연결 해제
    for (int i = 0; i < connected_device_count; i++) {
        if (connected_devices[i].is_connected) {
            esp_ble_gattc_close(connected_devices[i].gattc_if, connected_devices[i].conn_id);
        }
    }
    
    // 연결된 디바이스 목록 초기화
    memset(connected_devices, 0, sizeof(connected_devices));
    connected_device_count = 0;
    is_scanning = false;
    connection_in_progress = false;  // 연결 중 플래그 리셋
    
    // 초기화 상태 리셋
    ble_client_initialized = false;
}

// BLE 스캔 진행 중인지 확인
bool ble_client_is_scanning(void) {
    return is_scanning;
}

// BLE 연결 시도 중인지 확인
bool ble_client_is_connecting(void) {
    return connection_in_progress;
}

// nrf5340 데이터 수신 리스너 (기본 구현)
// 이 함수는 외부에서 재정의 가능합니다
__attribute__((weak)) void ble_client_receive_devices(const uint8_t* data, uint16_t length) {
    ESP_LOGE(TAG, "========== nrf5340 데이터 수신 ==========");
    ESP_LOGE(TAG, "데이터 길이: %d bytes", length);
    
    // 데이터를 16진수로 출력
    char hex_string[512] = {0};
    int offset = 0;
    for (int i = 0; i < length && offset < 510; i++) {
        offset += sprintf(&hex_string[offset], "%02x ", data[i]);
    }
    ESP_LOGE(TAG, "데이터 (hex): %s", hex_string);
    
    // ASCII 문자열로 출력 시도
    bool is_ascii = true;
    for (int i = 0; i < length; i++) {
        if (data[i] < 32 || data[i] > 126) {
            is_ascii = false;
            break;
        }
    }
    
    if (is_ascii) {
        char ascii_string[128] = {0};
        int copy_len = (length < sizeof(ascii_string) - 1) ? length : sizeof(ascii_string) - 1;
        strncpy(ascii_string, (char*)data, copy_len);
        ascii_string[copy_len] = '\0';
        ESP_LOGE(TAG, "데이터 (ASCII): %s", ascii_string);
    }
    
    ESP_LOGE(TAG, "===========================================");
}
