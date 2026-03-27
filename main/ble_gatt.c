// ble_gatt.c (Profile A only, Profile B removed)

#include "ble_gatt.h"
#include "wifi_manager.h"
#include "http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "string.h"
#include "stdlib.h"
#include "esp_mac.h"

static const char *TAG = "BLE_GATT";

/* 전역 변수 정의 */
char test_device_name[ESP_BLE_ADV_NAME_LEN_MAX] = "ESP32_S3";
esp_gatt_if_t current_gatts_if = ESP_GATT_IF_NONE;
uint16_t current_conn_id = 0;
uint16_t local_mtu = 23;
uint8_t char_value_read[CONFIG_EXAMPLE_CHAR_READ_DATA_LEN] = {0xDE,0xED,0xBE,0xEF};
uint8_t char1_str[] = {0x11,0x22,0x33};
uint16_t descr_value = 0x0;
esp_gatt_char_prop_t a_property = 0;
uint8_t adv_config_done = 0;
#define adv_config_flag      (1 << 0)
#define scan_rsp_config_flag (1 << 1)

#ifdef CONFIG_EXAMPLE_SET_RAW_ADV_DATA
static uint8_t raw_adv_data[] = {
    /* Flags */
    0x02, ESP_BLE_AD_TYPE_FLAG, 0x06,
    /* TX Power Level */
    0x02, ESP_BLE_AD_TYPE_TX_PWR, 0xEB,
    /* Complete 16-bit Service UUIDs */
    0x03, ESP_BLE_AD_TYPE_16SRV_CMPL, 0xAB, 0xCD
};

static uint8_t raw_scan_rsp_data[] = {
    0x0F, ESP_BLE_AD_TYPE_NAME_CMPL, 'E', 'S', 'P', '_', 'G', 'A', 'T', 'T', 'S', '_', 'D', 'E', 'M', 'O'
};
#else

/* 16비트 UUID 사용 (앱과 호환성을 위해) - 메인 서비스만 */
static uint8_t adv_service_uuid16[2] = {
    0xFF, 0x00   // Service UUID A (0x00FF)
};

bool ble_connected = false;

// BLE 초기화 상태 변수
static bool ble_initialized = false;

/* 광고 데이터 */
esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = false,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data =  NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 2,
    .p_service_uuid = adv_service_uuid16,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

/* scan response - 디바이스 이름이 제대로 표시되도록 수정 */
esp_ble_adv_data_t scan_rsp_data = {
    .set_scan_rsp = true,
    .include_name = true,
    .include_txpower = true,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data =  NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,  // UUID를 scan response에서 제거하여 이름 공간 확보
    .p_service_uuid = NULL, // UUID를 scan response에서 제거하여 이름 공간 확보
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

#endif /* CONFIG_SET_RAW_ADV_DATA */

esp_ble_adv_params_t adv_params = {
    .adv_int_min        = 0x20,
    .adv_int_max        = 0x40,
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/* gl_profile_tab: PROFILE_NUM은 헤더(ble_gatt.h)에 정의되어 있을 수 있으므로 그대로 사용.
   여기서는 PROFILE_A_APP_ID 항목만 초기화합니다. */
struct gatts_profile_inst gl_profile_tab[PROFILE_NUM] = {
    [PROFILE_A_APP_ID] = {
        .gatts_cb = gatts_profile_a_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,
    },
};

prepare_type_env_t a_prepare_write_env;

/* BLE 관련 전역 변수 */
char mac_address[18] = "00:00:00:00:00:00";

/* BLE로 받은 WiFi 데이터 저장용 전역 변수 */
char ble_wifi_id[64] = {0};
char ble_wifi_pw[64] = {0};
char ble_user_email[128] = {0};

/* BLE 데이터 수신 플래그 */
static bool new_ble_data_received = false;

/* BLE 데이터 수신 버퍼 (여러 패킷을 누적하여 저장) */
static char ble_receive_buffer[512] = {0};
static int ble_receive_buffer_len = 0;
static bool ble_receive_in_progress = false;  // 데이터 수신 중 여부

// BLE 데이터 수신 플래그 관리 함수
void set_new_ble_data_received(bool received) {
    new_ble_data_received = received;
}

bool get_new_ble_data_received(void) {
    return new_ble_data_received;
}

// BLE 수신 버퍼 초기화
void reset_ble_receive_buffer(void) {
    memset(ble_receive_buffer, 0, sizeof(ble_receive_buffer));
    ble_receive_buffer_len = 0;
    ble_receive_in_progress = false;
}

// 버퍼에서 줄바꿈 문자 제거
static void remove_newlines_from_buffer(void) {
    int write_idx = 0;
    for (int i = 0; i < ble_receive_buffer_len; i++) {
        if (ble_receive_buffer[i] != '\r' && ble_receive_buffer[i] != '\n') {
            ble_receive_buffer[write_idx++] = ble_receive_buffer[i];
        }
    }
    ble_receive_buffer_len = write_idx;
    ble_receive_buffer[ble_receive_buffer_len] = '\0';
}

esp_attr_value_t gatts_demo_char1_val = {
    .attr_max_len = GATTS_DEMO_CHAR_VAL_LEN_MAX,
    .attr_len     = sizeof(char1_str),
    .attr_value   = char1_str,
};

/* MAC address 초기화 */
void init_mac_address(void) {
    uint8_t mac[6];
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_BT);
    if (ret == ESP_OK) {
        snprintf(mac_address, sizeof(mac_address), "%02x:%02x:%02x:%02x:%02x:%02x",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        ESP_LOGI(TAG, "Bluetooth MAC Address: %s", mac_address);
    } else {
        ESP_LOGE(TAG, "Failed to read Bluetooth MAC address: %s", esp_err_to_name(ret));
        strcpy(mac_address, "00:00:00:00:00:00");
    }
}

/* MAC 반환 */
const char* get_mac_address(void) {
    return mac_address;
}

/* 서버는 직접 Notification 활성화 못함 (설명용 함수 유지) */
esp_err_t enable_notification(void) {
    if (!ble_connected) {
        ESP_LOGW(TAG, "BLE 연결이 없습니다. Notification 활성화 불가");
        return ESP_ERR_INVALID_STATE;
    }

    if (current_gatts_if == ESP_GATT_IF_NONE) {
        ESP_LOGW(TAG, "GATT 인터페이스가 초기화되지 않았습니다");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "서버는 직접 Notification을 활성화할 수 없습니다. 앱에서 CCCD에 0x0001을 써주세요.");
    return ESP_ERR_NOT_SUPPORTED;
}

/* BLE 메시지 전송 (Profile A만 사용) - 개선된 버전 */
esp_err_t send_ble_message(const char* message) {
    // 1. 메시지 유효성 검사
    if (!message) {
        ESP_LOGE(TAG, "메시지가 NULL입니다");
        return ESP_ERR_INVALID_ARG;
    }

    size_t message_len = strlen(message);
    if (message_len == 0) {
        ESP_LOGE(TAG, "메시지가 비어있습니다");
        return ESP_ERR_INVALID_ARG;
    }

    // 2. BLE 연결 상태를 더 엄격하게 확인
    if (!ble_connected) {
        ESP_LOGW(TAG, "BLE 연결이 활성화되지 않았습니다");
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t conn_id = gl_profile_tab[PROFILE_A_APP_ID].conn_id;
    uint16_t char_handle = gl_profile_tab[PROFILE_A_APP_ID].char_handle;
    esp_gatt_if_t gatts_if = gl_profile_tab[PROFILE_A_APP_ID].gatts_if;

    if (char_handle == 0 || gatts_if == ESP_GATT_IF_NONE) {
        ESP_LOGW(TAG, "BLE 정보가 올바르지 않습니다 (conn_id=%d, char_handle=%d, gatts_if=%d)",
                 conn_id, char_handle, gatts_if);
        return ESP_ERR_INVALID_STATE;
    }

    // 3. CCCD가 올바르게 설정되었는지 확인
    if (descr_value != 0x0001) {
        ESP_LOGW(TAG, "Notification이 활성화되지 않았습니다 (descr_value: 0x%04x)", descr_value);
        return ESP_ERR_INVALID_STATE;
    }

    // 4. 로그 출력
    ESP_LOGI(TAG, "BLE 메시지 전송 시도: %s", message);
    ESP_LOGI(TAG, "메시지 길이: %d", message_len);
    ESP_LOG_BUFFER_HEX(TAG, (uint8_t*)message, message_len);
    ESP_LOGI(TAG, "함수 호출 스택 확인 - send_ble_message 진입");

    // 5. BLE Notify 전송
    esp_err_t ret = esp_ble_gatts_send_indicate(
        gatts_if,
        conn_id,
        char_handle,
        message_len,
        (uint8_t*)message,
        false   // false = notify, true = indicate
    );

    // 6. 전송 결과 로그
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Profile A에서 전송 성공!");
    } else {
        ESP_LOGE(TAG, "Profile A 전송 실패: %s", esp_err_to_name(ret));
    }

    return ret;
}

/* 로컬 WiFi 정보 전송 (main.c에서 로드한 NVS 데이터) */
void send_local_wifi_info(esp_gatt_if_t gatts_if, uint16_t conn_id) {
    // main.c에서 로드한 NVS 데이터를 가져오기 위해 extern 선언 필요
    extern char* wifi_id_from_nvs;
    extern char* wifi_pw_from_nvs;
    extern char* user_email_from_nvs;
    extern char* mac_address_from_nvs;
    
    char wifi_info[512];
    int len = snprintf(wifi_info, sizeof(wifi_info), "local_id:%s,local_pw:%s,local_email:%s,local_mac:%s", 
                      wifi_id_from_nvs ? wifi_id_from_nvs : "",
                      wifi_pw_from_nvs ? wifi_pw_from_nvs : "",
                      user_email_from_nvs ? user_email_from_nvs : "",
                      mac_address_from_nvs ? mac_address_from_nvs : "");
    
    if (len >= sizeof(wifi_info)) {
        ESP_LOGE(TAG, "Local WiFi info too long, truncated");
        len = sizeof(wifi_info) - 1;
        wifi_info[len] = '\0';
    }
    
    // Add newline at the end
    if (len < sizeof(wifi_info) - 1) {
        wifi_info[len] = '\n';
        wifi_info[len + 1] = '\0';
        len++;
    }
    
    esp_err_t ret = esp_ble_gatts_send_indicate(gatts_if, conn_id, 
                                                gl_profile_tab[PROFILE_A_APP_ID].char_handle,
                                                len, 
                                                (uint8_t*)wifi_info, 
                                                false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send local WiFi info: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Local WiFi info sent successfully");
    }
}

/* NVS WiFi 정보 전송 */
void send_nvs_wifi_info(esp_gatt_if_t gatts_if, uint16_t conn_id) {
    // NVS에서 직접 로드하는 대신 main.c의 local 변수들 사용
    extern char* local_wifi_id;
    extern char* local_wifi_pw;
    extern char* local_user_email;
    extern char* local_mac_address;
    
    char wifi_info[512];
    int len = snprintf(wifi_info, sizeof(wifi_info), "nvs_id:%s,nvs_pw:%s,nvs_email:%s,nvs_mac:%s", 
                      local_wifi_id ? local_wifi_id : "",
                      local_wifi_pw ? local_wifi_pw : "",
                      local_user_email ? local_user_email : "",
                      local_mac_address ? local_mac_address : "");
    
    if (len >= sizeof(wifi_info)) {
        ESP_LOGE(TAG, "NVS WiFi info too long, truncated");
        len = sizeof(wifi_info) - 1;
        wifi_info[len] = '\0';
    }
    
    // Add newline at the end
    if (len < sizeof(wifi_info) - 1) {
        wifi_info[len] = '\n';
        wifi_info[len + 1] = '\0';
        len++;
    }
    
    esp_err_t ret = esp_ble_gatts_send_indicate(gatts_if, conn_id, 
                                                gl_profile_tab[PROFILE_A_APP_ID].char_handle,
                                                len, 
                                                (uint8_t*)wifi_info, 
                                                false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send NVS WiFi info: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "NVS WiFi info sent successfully");
    }
    
}

/* BLE WiFi 정보 전송 */
void send_ble_wifi_info(esp_gatt_if_t gatts_if, uint16_t conn_id) {
    char wifi_info[512];
    int len = snprintf(wifi_info, sizeof(wifi_info), "ble_id:%s,ble_pw:%s,ble_email:%s", 
                      ble_wifi_id, ble_wifi_pw, ble_user_email);
    
    if (len >= sizeof(wifi_info)) {
        ESP_LOGE(TAG, "BLE WiFi info too long, truncated");
        len = sizeof(wifi_info) - 1;
        wifi_info[len] = '\0';
    }
    
    // Add newline at the end
    if (len < sizeof(wifi_info) - 1) {
        wifi_info[len] = '\n';
        wifi_info[len + 1] = '\0';
        len++;
    }
    
    esp_err_t ret = esp_ble_gatts_send_indicate(gatts_if, conn_id, 
                                                gl_profile_tab[PROFILE_A_APP_ID].char_handle,
                                                len, 
                                                (uint8_t*)wifi_info, 
                                                false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send BLE WiFi info: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "BLE WiFi info sent successfully");
    }
}

/* BLE 초기화 상태 확인 함수 */
bool is_ble_initialized(void) {
    return ble_initialized;
}

/* BLE 초기화 (Profile A만 등록) */
esp_err_t ble_init(void)
{
    if (ble_initialized) {
        ESP_LOGW(TAG, "BLE already initialized");
        return ESP_OK;
    }

    esp_err_t ret;

    ESP_LOGI(TAG, "=== BLE 초기화 시작 ===");

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_log_level_set("BLE_INIT", ESP_LOG_NONE);
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "%s initialize controller failed: %s\n", __func__, esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "%s enable controller failed: %s\n", __func__, esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG, "%s init bluetooth failed: %s\n", __func__, esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "%s enable bluetooth failed: %s\n", __func__, esp_err_to_name(ret));
        return ret;
    }

    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "gatts register error, error code = %x", ret);
        return ret;
    }

    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "gap register error, error code = %x", ret);
        return ret;
    }

    /* PROFILE_A 만 등록 */
    ret = esp_ble_gatts_app_register(PROFILE_A_APP_ID);
    if (ret) {
        ESP_LOGE(TAG, "gatts app register error, error code = %x", ret);
        return ret;
    }

    esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(500);
    if (local_mtu_ret) {
        ESP_LOGE(TAG, "set local MTU failed, error code = %x", local_mtu_ret);
        return local_mtu_ret;
    }

    ble_initialized = true;
    ESP_LOGI(TAG, "=== BLE 초기화 완료 ===");
    return ESP_OK;
}

/* 광고 시작 */
esp_err_t ble_start_advertising(void)
{
    if (!ble_initialized) {
        ESP_LOGE(TAG, "BLE not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "=== BLE 광고 시작 ===");

    esp_err_t ret = esp_ble_gap_start_advertising(&adv_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BLE 광고 시작 실패: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "=== BLE 광고 시작됨 ===");
    return ESP_OK;
}



// BLE 연결 및 광고 끊기 함수
esp_err_t ble_disconnect(void) {
    esp_err_t ret = ESP_OK;
    
    // BLE 연결 끊기
    if (gl_profile_tab[PROFILE_A_APP_ID].conn_id != 0) {
        ret = esp_ble_gap_disconnect(gl_profile_tab[PROFILE_A_APP_ID].conn_id);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "BLE 연결 끊기 성공");
        } else {
            ESP_LOGE(TAG, "BLE 연결 끊기 실패: %s", esp_err_to_name(ret));
        }
    }
    
    // BLE 광고 중지
    esp_err_t adv_ret = esp_ble_gap_stop_advertising();
    if (adv_ret == ESP_OK) {
        ESP_LOGI(TAG, "BLE 광고 중지 성공");
    } else {
        ESP_LOGE(TAG, "BLE 광고 중지 실패: %s", esp_err_to_name(ret));
    }
    
    // 연결 상태 초기화
    gl_profile_tab[PROFILE_A_APP_ID].conn_id = 0;
    gl_profile_tab[PROFILE_A_APP_ID].gatts_if = ESP_GATT_IF_NONE;
    ble_connected = false;
    
    return ret;
}

// BLE 서버 완전 종료 함수 (클라이언트 모드로 전환 시 사용)
esp_err_t ble_complete_deinit(void) {
    ESP_LOGI(TAG, "=== BLE 서버 완전 종료 시작 ===");
    
    // 1. BLE 연결 및 광고 끊기
    ble_disconnect();
    // (보호) 스캔/광고 강제 중지 시도
    esp_err_t tmp;
    tmp = esp_ble_gap_stop_scanning();
    if (tmp != ESP_OK && tmp != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "스캔 중지 경고: %s", esp_err_to_name(tmp));
    }
    tmp = esp_ble_gap_stop_advertising();
    if (tmp != ESP_OK && tmp != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "광고 중지 경고: %s", esp_err_to_name(tmp));
    }
    
    // 해제 전 안정화 대기
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // 2. Bluedroid 종료
    esp_err_t ret = esp_bluedroid_disable();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Bluedroid 비활성화 경고: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Bluedroid 비활성화 완료");
    }
    
    // Bluedroid 해제 대기
    vTaskDelay(pdMS_TO_TICKS(500));
    // 2-1. Bluedroid deinit
    ret = esp_bluedroid_deinit();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Bluedroid 해제 경고: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Bluedroid 해제 완료");
    }
    
    // 3. BT 컨트롤러 비활성화
    ret = esp_bt_controller_disable();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "BT 컨트롤러 비활성화 경고: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "BT 컨트롤러 비활성화 완료");
    }
    
    // 컨트롤러 비활성화 대기
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // 4. BT 컨트롤러 해제
    ret = esp_bt_controller_deinit();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "BT 컨트롤러 해제 경고: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "BT 컨트롤러 해제 완료");
    }
    // 4-1. Classic BT 메모리 해제(선택, 권장)
    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Classic BT 메모리 해제 경고: %s", esp_err_to_name(ret));
    }
    
    // 최종 안정화 대기
    vTaskDelay(pdMS_TO_TICKS(500));
    
    ble_initialized = false;
    ESP_LOGI(TAG, "=== BLE 서버 완전 종료 완료 ===");
    
    return ESP_OK;
}

/* GAP 이벤트 핸들러 (변경 없음) */
void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
#ifdef CONFIG_EXAMPLE_SET_RAW_ADV_DATA
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        adv_config_done &= (~adv_config_flag);
        if (adv_config_done==0){
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        adv_config_done &= (~scan_rsp_config_flag);
        if (adv_config_done==0){
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
#else
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        adv_config_done &= (~adv_config_flag);
        if (adv_config_done == 0){
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        adv_config_done &= (~scan_rsp_config_flag);
        if (adv_config_done == 0){
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
#endif
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "=== 광고 시작 실패 ===");
            ESP_LOGE(TAG, "상태: %d", param->adv_start_cmpl.status);
            break;
        }
        ESP_LOGI(TAG, "=== 광고 시작 성공! ===");
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising stop failed, status %d", param->adv_stop_cmpl.status);
        }
        ESP_LOGI(TAG, "Advertising stop successfully");
        break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        break;
    case ESP_GAP_BLE_SET_PKT_LENGTH_COMPLETE_EVT:
        break;
    default:
        break;
    }
}

/* Write / exec helpers (unchanged) */
void example_write_event_env(esp_gatt_if_t gatts_if, prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param){
    esp_gatt_status_t status = ESP_GATT_OK;
    if (param->write.need_rsp){
        if (param->write.is_prep) {
            if (param->write.offset > PREPARE_BUF_MAX_SIZE) {
                status = ESP_GATT_INVALID_OFFSET;
            } else if ((param->write.offset + param->write.len) > PREPARE_BUF_MAX_SIZE) {
                status = ESP_GATT_INVALID_ATTR_LEN;
            }
            if (status == ESP_GATT_OK && prepare_write_env->prepare_buf == NULL) {
                prepare_write_env->prepare_buf = (uint8_t *)malloc(PREPARE_BUF_MAX_SIZE*sizeof(uint8_t));
                prepare_write_env->prepare_len = 0;
                if (prepare_write_env->prepare_buf == NULL) {
                    ESP_LOGE(TAG, "Gatt_server prep no mem\n");
                    status = ESP_GATT_NO_RESOURCES;
                }
            }

            esp_gatt_rsp_t *gatt_rsp = (esp_gatt_rsp_t *)malloc(sizeof(esp_gatt_rsp_t));
            gatt_rsp->attr_value.len = param->write.len;
            gatt_rsp->attr_value.handle = param->write.handle;
            gatt_rsp->attr_value.offset = param->write.offset;
            gatt_rsp->attr_value.auth_req = ESP_GATT_AUTH_REQ_NONE;
            memcpy(gatt_rsp->attr_value.value, param->write.value, param->write.len);
            esp_err_t response_err = esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, status, gatt_rsp);
            if (response_err != ESP_OK){
               ESP_LOGE(TAG, "Send response error\n");
            }
            free(gatt_rsp);
            if (status != ESP_GATT_OK){
                return;
            }
            memcpy(prepare_write_env->prepare_buf + param->write.offset,
                   param->write.value,
                   param->write.len);
            prepare_write_env->prepare_len += param->write.len;

        }else{
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, status, NULL);
        }
    }
}

void example_exec_write_event_env(prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param){
    if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC){
        esp_log_buffer_hex(TAG, prepare_write_env->prepare_buf, prepare_write_env->prepare_len);
    }else{
        ESP_LOGI(TAG,"Prepare write cancel");
    }
    if (prepare_write_env->prepare_buf) {
        free(prepare_write_env->prepare_buf);
        prepare_write_env->prepare_buf = NULL;
    }
    prepare_write_env->prepare_len = 0;
}

/* Profile A event handler (single profile) */
void gatts_profile_a_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    switch (event) {
    case ESP_GATTS_REG_EVT:
        gl_profile_tab[PROFILE_A_APP_ID].service_id.is_primary = true;
        gl_profile_tab[PROFILE_A_APP_ID].service_id.id.inst_id = 0x00;
        gl_profile_tab[PROFILE_A_APP_ID].service_id.id.uuid.len = ESP_UUID_LEN_16;
        gl_profile_tab[PROFILE_A_APP_ID].service_id.id.uuid.uuid.uuid16 = GATTS_SERVICE_UUID_TEST_A;

        esp_err_t set_dev_name_ret = esp_ble_gap_set_device_name(test_device_name);
        if (set_dev_name_ret){
            ESP_LOGE(TAG, "set device name failed, error code = %x", set_dev_name_ret);
        }

#ifdef CONFIG_EXAMPLE_SET_RAW_ADV_DATA
        esp_err_t raw_adv_ret = esp_ble_gap_config_adv_data_raw(raw_adv_data, sizeof(raw_adv_data));
        if (raw_adv_ret){
            ESP_LOGE(TAG, "config raw adv data failed, error code = %x ", raw_adv_ret);
        }
        adv_config_done |= adv_config_flag;
        esp_err_t raw_scan_ret = esp_ble_gap_config_scan_rsp_data_raw(raw_scan_rsp_data, sizeof(raw_scan_rsp_data));
        if (raw_scan_ret){
            ESP_LOGE(TAG, "config raw scan rsp data failed, error code = %x", raw_scan_ret);
        }
        adv_config_done |= scan_rsp_config_flag;
#else
        esp_err_t ret = esp_ble_gap_config_adv_data(&adv_data);
        if (ret){
            ESP_LOGE(TAG, "config adv data failed, error code = %x", ret);
        }
        adv_config_done |= adv_config_flag;
        ret = esp_ble_gap_config_adv_data(&scan_rsp_data);
        if (ret){
            ESP_LOGE(TAG, "config scan response data failed, error code = %x", ret);
        }
        adv_config_done |= scan_rsp_config_flag;
#endif

        ESP_LOGI(TAG, "서비스 생성 중...");
        esp_ble_gatts_create_service(gatts_if, &gl_profile_tab[PROFILE_A_APP_ID].service_id, GATTS_NUM_HANDLE_TEST_A);
        break;

    case ESP_GATTS_READ_EVT: {
        ESP_LOGI(TAG,
                    "Characteristic read request: conn_id=%d, trans_id=%" PRIu32 ", handle=%d, is_long=%d, offset=%d, need_rsp=%d",
                    param->read.conn_id, param->read.trans_id, param->read.handle,
                    param->read.is_long, param->read.offset, param->read.need_rsp);

        if (!param->read.need_rsp) {
            return;
        }

        esp_gatt_rsp_t rsp;
        memset(&rsp, 0, sizeof(esp_gatt_rsp_t));
        rsp.attr_value.handle = param->read.handle;

        if (param->read.handle == gl_profile_tab[PROFILE_A_APP_ID].descr_handle) {
            memcpy(rsp.attr_value.value, &descr_value, 2);
            rsp.attr_value.len = 2;
            esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id, ESP_GATT_OK, &rsp);
            return;
        }

        if (param->read.handle == gl_profile_tab[PROFILE_A_APP_ID].char_handle) {
            uint16_t offset = param->read.offset;

            if (param->read.is_long && offset > CONFIG_EXAMPLE_CHAR_READ_DATA_LEN) {
                ESP_LOGW(TAG, "Read offset (%d) out of range (0-%d)", offset, CONFIG_EXAMPLE_CHAR_READ_DATA_LEN);
                rsp.attr_value.len = 0;
                esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id, ESP_GATT_INVALID_OFFSET, &rsp);
                return;
            }

            uint16_t mtu_size = local_mtu - 1;
            uint16_t send_len = (CONFIG_EXAMPLE_CHAR_READ_DATA_LEN - offset > mtu_size) ? mtu_size : (CONFIG_EXAMPLE_CHAR_READ_DATA_LEN - offset);

            memcpy(rsp.attr_value.value, &char_value_read[offset], send_len);
            rsp.attr_value.len = send_len;

            esp_err_t err = esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id, ESP_GATT_OK, &rsp);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send response: %s", esp_err_to_name(err));
            }
        }
        break;
    }

    case ESP_GATTS_WRITE_EVT: {
        if (!param->write.is_prep){
            ESP_LOGI(TAG, "BLE 데이터 수신: %.*s", param->write.len, param->write.value);

            /* 전역 연결 상태 업데이트 - Profile A에서만 관리 */
            if (!ble_connected) {
                ble_connected = true;
            }
            current_conn_id = param->write.conn_id;
            current_gatts_if = gatts_if;
            
            /* Profile A 정보도 업데이트 */
            gl_profile_tab[PROFILE_A_APP_ID].conn_id = param->write.conn_id;
            gl_profile_tab[PROFILE_A_APP_ID].gatts_if = gatts_if;
            
            // ESP_LOGI(TAG, "WRITE_EVT에서 Profile A 업데이트 - conn_id: %d, gatts_if: %d", 
            //          gl_profile_tab[PROFILE_A_APP_ID].conn_id, gl_profile_tab[PROFILE_A_APP_ID].gatts_if);

            /* CCCD write 처리 체크 (먼저 처리 후 return) */
            if (gl_profile_tab[PROFILE_A_APP_ID].descr_handle == param->write.handle && param->write.len == 2){
                descr_value = param->write.value[1]<<8 | param->write.value[0];
                if (descr_value == 0x0001){
                    if (a_property & ESP_GATT_CHAR_PROP_BIT_NOTIFY){
                        ESP_LOGI(TAG, "Notification enable");
                    }
                } else if (descr_value == 0x0002){
                    if (a_property & ESP_GATT_CHAR_PROP_BIT_INDICATE){
                        uint8_t indicate_data[15];
                        for (int i = 0; i < sizeof(indicate_data); ++i) indicate_data[i] = i%0xff;
                        esp_ble_gatts_send_indicate(gatts_if, param->write.conn_id, gl_profile_tab[PROFILE_A_APP_ID].char_handle,
                                                    sizeof(indicate_data), indicate_data, true);
                    }
                } else if (descr_value == 0x0000){
                    /* disable */
                } else {
                    ESP_LOGE(TAG, "Unknown descriptor value");
                    ESP_LOG_BUFFER_HEX(TAG, param->write.value, param->write.len);
                }
                
                example_write_event_env(gatts_if, &a_prepare_write_env, param);
                break;
            }

            /* 받은 데이터를 문자열로 변환 */
            char received_data[256] = {0};
            int copy_len = (param->write.len < 255) ? param->write.len : 255;
            memcpy(received_data, param->write.value, copy_len);
            received_data[copy_len] = '\0';

            /* s: 로 시작하면 데이터 수신 시작 */
            if (strncmp(received_data, "s:", 2) == 0) {
                // 버퍼 초기화하고 시작
                reset_ble_receive_buffer();
                
                // "s:" 뒤의 데이터를 버퍼에 추가
                if (copy_len > 2) {
                    int data_len = copy_len - 2;
                    int available_space = sizeof(ble_receive_buffer) - 1 - ble_receive_buffer_len;
                    int append_len = (data_len < available_space) ? data_len : available_space;
                    
                    if (append_len > 0) {
                        memcpy(ble_receive_buffer, received_data + 2, append_len);
                        ble_receive_buffer_len = append_len;
                        ble_receive_buffer[ble_receive_buffer_len] = '\0';
                        ble_receive_in_progress = true;
                        
                        ESP_LOGI(TAG, "데이터 수신 시작. 버퍼: %s", ble_receive_buffer);
                    }
                } else {
                    ble_receive_in_progress = true;
                    ESP_LOGI(TAG, "데이터 수신 시작 (초기 데이터 없음)");
                }
            }
            /* e: 로 시작하면 데이터 수신 종료 및 파싱 */
            else if (strncmp(received_data, "e:", 2) == 0 && ble_receive_in_progress) {
                // "e:" 뒤의 데이터를 버퍼에 추가
                if (copy_len > 2) {
                    int data_len = copy_len - 2;
                    int available_space = sizeof(ble_receive_buffer) - 1 - ble_receive_buffer_len;
                    int append_len = (data_len < available_space) ? data_len : available_space;
                    
                    if (append_len > 0) {
                        memcpy(ble_receive_buffer + ble_receive_buffer_len, received_data + 2, append_len);
                        ble_receive_buffer_len += append_len;
                        ble_receive_buffer[ble_receive_buffer_len] = '\0';
                    }
                }
                
                // 완전한 데이터 수신 완료
                ESP_LOGI(TAG, "데이터 수신 완료. 총 길이: %d", ble_receive_buffer_len);
                ESP_LOGI(TAG, "수신된 데이터 (줄바꿈 포함): %s", ble_receive_buffer);
                
                /* 줄바꿈 문자 제거 */
                remove_newlines_from_buffer();
                
                ESP_LOGI(TAG, "줄바꿈 제거 후 데이터: %s", ble_receive_buffer);
                ESP_LOGI(TAG, "줄바꿈 제거 후 길이: %d", ble_receive_buffer_len);
                
                /* Parse WiFi command */
                bool wifi_parsed = parse_wifi_command(ble_receive_buffer, ble_receive_buffer_len);
                
                if (wifi_parsed) {
                    // BLE로 받은 WiFi 데이터를 전역 변수에 저장
                    strncpy(ble_wifi_id, wifi_id, sizeof(ble_wifi_id) - 1);
                    ble_wifi_id[sizeof(ble_wifi_id) - 1] = '\0';
                    
                    strncpy(ble_wifi_pw, wifi_pw, sizeof(ble_wifi_pw) - 1);
                    ble_wifi_pw[sizeof(ble_wifi_pw) - 1] = '\0';
                    
                    strncpy(ble_user_email, user_email, sizeof(ble_user_email) - 1);
                    ble_user_email[sizeof(ble_user_email) - 1] = '\0';
                    
                    // 새로운 BLE 데이터 수신 플래그 설정
                    set_new_ble_data_received(true);
                    
                    ESP_LOGI(TAG, "BLE WiFi 데이터 저장됨 - ID: %s, PW: %s, Email: %s", 
                             ble_wifi_id, ble_wifi_pw, ble_user_email);
                }
                
                // 버퍼 초기화
                reset_ble_receive_buffer();
            }
            /* 중간 데이터: 수신 중이면 버퍼에 추가 */
            else if (ble_receive_in_progress) {
                int available_space = sizeof(ble_receive_buffer) - 1 - ble_receive_buffer_len;
                int append_len = (copy_len < available_space) ? copy_len : available_space;
                
                if (append_len > 0) {
                    memcpy(ble_receive_buffer + ble_receive_buffer_len, received_data, append_len);
                    ble_receive_buffer_len += append_len;
                    ble_receive_buffer[ble_receive_buffer_len] = '\0';
                    
                    ESP_LOGI(TAG, "버퍼에 데이터 추가됨. 총 길이: %d", ble_receive_buffer_len);
                } else {
                    ESP_LOGW(TAG, "버퍼가 가득 참! 일부 데이터 손실");
                }
            }
            /* 일반 명령어 처리 (checklocal, checknvs, checkble) */
            else {
                char* trimmed_cmd = received_data;
                // 앞뒤 공백 제거
                while (*trimmed_cmd == ' ' || *trimmed_cmd == '\t' || *trimmed_cmd == '\r' || *trimmed_cmd == '\n') {
                    trimmed_cmd++;
                }
                char* end = trimmed_cmd + strlen(trimmed_cmd) - 1;
                while (end > trimmed_cmd && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
                    *end = '\0';
                    end--;
                }

                if (strcmp(trimmed_cmd, "checklocal") == 0) {
                    send_local_wifi_info(gatts_if, param->write.conn_id);
                } else if (strcmp(trimmed_cmd, "checknvs") == 0) {
                    send_nvs_wifi_info(gatts_if, param->write.conn_id);
                } else if (strcmp(trimmed_cmd, "checkble") == 0) {
                    send_ble_wifi_info(gatts_if, param->write.conn_id);
                }
            }
        }
        example_write_event_env(gatts_if, &a_prepare_write_env, param);
        break;
    }

    case ESP_GATTS_EXEC_WRITE_EVT:
        ESP_LOGI(TAG,"Execute write");
        esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
        example_exec_write_event_env(&a_prepare_write_env, param);
        break;

    case ESP_GATTS_MTU_EVT:
        local_mtu = param->mtu.mtu;
        break;

    case ESP_GATTS_CREATE_EVT:
        // ESP_LOGI(TAG, "Service create, status %d, service_handle %d", param->create.status, param->create.service_handle);
        gl_profile_tab[PROFILE_A_APP_ID].service_handle = param->create.service_handle;
        gl_profile_tab[PROFILE_A_APP_ID].char_uuid.len = ESP_UUID_LEN_16;
        gl_profile_tab[PROFILE_A_APP_ID].char_uuid.uuid.uuid16 = GATTS_CHAR_UUID_TEST_A;

        esp_ble_gatts_start_service(gl_profile_tab[PROFILE_A_APP_ID].service_handle);
        a_property = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
        esp_err_t add_char_ret = esp_ble_gatts_add_char(gl_profile_tab[PROFILE_A_APP_ID].service_handle, &gl_profile_tab[PROFILE_A_APP_ID].char_uuid,
                                                        ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                                        a_property,
                                                        &gatts_demo_char1_val, NULL);
        if (add_char_ret){
            ESP_LOGE(TAG, "add char failed, error code =%x",add_char_ret);
        }
        break;

    case ESP_GATTS_ADD_CHAR_EVT: {
        uint16_t length = 0;
        const uint8_t *prf_char;

        // ESP_LOGI(TAG, "Characteristic add, status %d, attr_handle %d, service_handle %d",
        //         param->add_char.status, param->add_char.attr_handle, param->add_char.service_handle);
        gl_profile_tab[PROFILE_A_APP_ID].char_handle = param->add_char.attr_handle;
        gl_profile_tab[PROFILE_A_APP_ID].descr_uuid.len = ESP_UUID_LEN_16;
        gl_profile_tab[PROFILE_A_APP_ID].descr_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
        esp_err_t get_attr_ret = esp_ble_gatts_get_attr_value(param->add_char.attr_handle,  &length, &prf_char);
        if (get_attr_ret == ESP_FAIL){
            ESP_LOGE(TAG, "ILLEGAL HANDLE");
        }

        // ESP_LOGI(TAG, "the gatts demo char length = %x\n", length);
        for(int i = 0; i < length; i++){
            // ESP_LOGI(TAG, "prf_char[%x] =%x\n",i,prf_char[i]);
        }
        esp_err_t add_descr_ret = esp_ble_gatts_add_char_descr(gl_profile_tab[PROFILE_A_APP_ID].service_handle, &gl_profile_tab[PROFILE_A_APP_ID].descr_uuid,
                                                                ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, NULL, NULL);
        if (add_descr_ret){
            ESP_LOGE(TAG, "add char descr failed, error code =%x", add_descr_ret);
        }
        break;
    }

    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
        gl_profile_tab[PROFILE_A_APP_ID].descr_handle = param->add_char_descr.attr_handle;
        // ESP_LOGI(TAG, "Descriptor add, status %d, attr_handle %d, service_handle %d",
        //          param->add_char_descr.status, param->add_char_descr.attr_handle, param->add_char_descr.service_handle);
        break;

    case ESP_GATTS_START_EVT:
        // ESP_LOGI(TAG, "Service start, status %d, service_handle %d",
        //          param->start.status, param->start.service_handle);
        if (param->start.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "=== 서비스 시작 완료, 광고 시작 ===");
            esp_err_t adv_ret = esp_ble_gap_start_advertising(&adv_params);
            if (adv_ret != ESP_OK) {
                ESP_LOGE(TAG, "광고 시작 실패: %s", esp_err_to_name(adv_ret));
            } else {
                ESP_LOGI(TAG, "=== 광고 시작 성공! 앱에서 연결 가능 ===");
            }
        } else {
            ESP_LOGE(TAG, "서비스 시작 실패: %d", param->start.status);
        }
        break;

    case ESP_GATTS_CONNECT_EVT: {
        ESP_LOGI(TAG, "*** BLE 연결됨 ***");
        ESP_LOGI(TAG, "conn_id %u, remote "ESP_BD_ADDR_STR"",
                 param->connect.conn_id, ESP_BD_ADDR_HEX(param->connect.remote_bda));

        gl_profile_tab[PROFILE_A_APP_ID].conn_id = param->connect.conn_id;
        gl_profile_tab[PROFILE_A_APP_ID].gatts_if = gatts_if;

        /* 전역 상태 업데이트 (Profile A에서만 관리) */
        current_conn_id = param->connect.conn_id;
        current_gatts_if = gatts_if;
        ble_connected = true;

        // ESP_LOGI(TAG, "Profile A 설정 완료 - conn_id: %d, gatts_if: %d", param->connect.conn_id, gatts_if);

        esp_ble_conn_update_params_t conn_params = {0};
        memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        conn_params.latency = 0;
        conn_params.max_int = 0x20;
        conn_params.min_int = 0x10;
        conn_params.timeout = 400;
        esp_ble_gap_update_conn_params(&conn_params);
        break;
    }

    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(TAG, "*** BLE 연결 해제됨 ***");
        ESP_LOGI(TAG, "remote "ESP_BD_ADDR_STR", reason 0x%02x",
                 ESP_BD_ADDR_HEX(param->disconnect.remote_bda), param->disconnect.reason);
        current_conn_id = 0;
        current_gatts_if = ESP_GATT_IF_NONE;
        ble_connected = false;
        esp_ble_gap_start_advertising(&adv_params);
        local_mtu = 23;
        break;

    case ESP_GATTS_CONF_EVT:
        ESP_LOGI(TAG, "Confirm receive, status %d, attr_handle %d", param->conf.status, param->conf.handle);
        if (param->conf.status != ESP_GATT_OK){
            esp_log_buffer_hex(TAG, param->conf.value, param->conf.len);
        }
        break;

    default:
        break;
    }
}

/* GATTS event handler */
void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            gl_profile_tab[param->reg.app_id].gatts_if = gatts_if;
        } else {
            ESP_LOGI(TAG, "Reg app failed, app_id %04x, status %d\n",
                    param->reg.app_id,
                    param->reg.status);
            return;
        }
    }

    /* Call profile callback(s). We only have meaningful callback for PROFILE_A_APP_ID */
    int idx;
    for (idx = 0; idx < PROFILE_NUM; idx++) {
        if (gatts_if == ESP_GATT_IF_NONE || gatts_if == gl_profile_tab[idx].gatts_if) {
            if (gl_profile_tab[idx].gatts_cb) {
                gl_profile_tab[idx].gatts_cb(event, gatts_if, param);
            }
        }
    }
}
