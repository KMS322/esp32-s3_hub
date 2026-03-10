#ifndef BLE_GATT_H
#define BLE_GATT_H

#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"

// BLE 관련 상수
#define GATTS_SERVICE_UUID_TEST_A   0x00FF
#define GATTS_CHAR_UUID_TEST_A      0xFF01
#define GATTS_DESCR_UUID_TEST_A     0x3333
#define GATTS_NUM_HANDLE_TEST_A     4

#define GATTS_SERVICE_UUID_TEST_B   0x00EE
#define GATTS_CHAR_UUID_TEST_B      0xEE01
#define GATTS_DESCR_UUID_TEST_B     0x2222
#define GATTS_NUM_HANDLE_TEST_B     4

// 외부 변수 선언
extern char test_device_name[ESP_BLE_ADV_NAME_LEN_MAX];

#define PROFILE_NUM 2
#define PROFILE_A_APP_ID 0
#define PROFILE_B_APP_ID 1

#define GATTS_DEMO_CHAR_VAL_LEN_MAX 0x40
#define PREPARE_BUF_MAX_SIZE 1024

// 구조체 정의
struct gatts_profile_inst {
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_handle;
    esp_gatt_srvc_id_t service_id;
    uint16_t char_handle;
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t perm;
    esp_gatt_char_prop_t property;
    uint16_t descr_handle;
    esp_bt_uuid_t descr_uuid;
};

typedef struct {
    uint8_t                 *prepare_buf;
    int                     prepare_len;
} prepare_type_env_t;

// 전역 변수 선언
extern char test_device_name[ESP_BLE_ADV_NAME_LEN_MAX];
extern bool ble_connected;
extern esp_gatt_if_t current_gatts_if;
extern uint16_t current_conn_id;

// BLE로 받은 WiFi 데이터
extern char ble_wifi_id[64];
extern char ble_wifi_pw[64];
extern char ble_user_email[128];
extern struct gatts_profile_inst gl_profile_tab[PROFILE_NUM];
extern prepare_type_env_t a_prepare_write_env;
extern prepare_type_env_t b_prepare_write_env;
extern uint16_t local_mtu;
extern uint8_t char_value_read[CONFIG_EXAMPLE_CHAR_READ_DATA_LEN];
extern uint8_t char1_str[];
extern uint16_t descr_value;
extern esp_gatt_char_prop_t a_property;
extern esp_gatt_char_prop_t b_property;
extern esp_attr_value_t gatts_demo_char1_val;
extern uint8_t adv_config_done;
extern esp_ble_adv_data_t adv_data;
extern esp_ble_adv_data_t scan_rsp_data;
extern esp_ble_adv_params_t adv_params;

// 함수 선언
void init_mac_address(void);
const char* get_mac_address(void);

// BLE 메시지 전송 함수
esp_err_t send_ble_message(const char* message);
esp_err_t ble_init(void);
esp_err_t ble_start_advertising(void);
esp_err_t ble_disconnect(void);
esp_err_t ble_complete_deinit(void);
bool is_ble_initialized(void);
void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
void gatts_profile_a_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
void gatts_profile_b_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
void example_write_event_env(esp_gatt_if_t gatts_if, prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param);
void example_exec_write_event_env(prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param);

// BLE 메시지 전송 관련 함수
esp_err_t send_ble_message(const char* message);
esp_err_t enable_notification(void);

// BLE 데이터 수신 플래그 관리 함수
void set_new_ble_data_received(bool received);
bool get_new_ble_data_received(void);

// WiFi 정보 전송 함수들
void send_local_wifi_info(esp_gatt_if_t gatts_if, uint16_t conn_id);
void send_nvs_wifi_info(esp_gatt_if_t gatts_if, uint16_t conn_id);
void send_ble_wifi_info(esp_gatt_if_t gatts_if, uint16_t conn_id);

#endif // BLE_GATT_H
