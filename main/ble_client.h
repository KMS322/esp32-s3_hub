#ifndef BLE_CLIENT_H
#define BLE_CLIENT_H

#include "esp_err.h"
#include "esp_gattc_api.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// 연결할 디바이스 정보 구조체
typedef struct {
    char device_name[32];
    uint8_t mac_address[6];
    bool is_connected;
    uint16_t conn_id;
    esp_gatt_if_t gattc_if;
} nrf5340_device_t;

// BLE 클라이언트 초기화
esp_err_t ble_client_init(void);

// BLE 클라이언트 초기화 여부 확인
bool ble_client_is_initialized(void);

// 특정 디바이스 스캔 시작
esp_err_t ble_client_scan_for_device(const char* device_name, const uint8_t* target_mac);

// BLE 스캔 함수
esp_err_t ble_client_scan_devices(const char* target_device_name, const uint8_t* target_mac_addresses, int mac_count);

// 연결된 디바이스 목록 가져오기
int ble_client_get_connected_devices(nrf5340_device_t* devices, int max_devices);

// 특정 디바이스 연결 상태 확인
bool ble_client_is_device_connected(const uint8_t* mac_address);

// BLE 클라이언트 정리
void ble_client_cleanup(void);

// 연결된 디바이스가 있는지 확인
bool ble_client_has_connected_devices(void);

// BLE 스캔 진행 중인지 확인
bool ble_client_is_scanning(void);

// BLE 연결 시도 중인지 확인
bool ble_client_is_connecting(void);

// nrf5340 데이터 수신 리스너 (외부에서 구현 가능)
void ble_client_receive_devices(const uint8_t* data, uint16_t length);

#endif // BLE_CLIENT_H