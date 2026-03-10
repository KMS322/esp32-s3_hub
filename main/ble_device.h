#ifndef BLE_DEVICE_H
#define BLE_DEVICE_H

#include "esp_err.h"
#include <stdbool.h>

// 전역 변수 선언 (외부 접근 가능)
extern uint8_t found_device_mac[6];
extern bool device_found;

// BLE 클라이언트 초기화
esp_err_t ble_device_init(void);

// BLE 초기화 여부 확인
bool ble_device_is_init_func(void);

// BLE 스캔 함수
// device_name: 디바이스 이름 (NULL이면 이름 필터링 안함)
// mac_address: MAC 주소 "AA:BB:CC:DD:EE:FF" 형식 (NULL이면 MAC 필터링 안함)
// 반환값: 찾았으면 true, 못 찾으면 false (최대 10초)
bool ble_device_scan(const char* device_name, const char* mac_address);

// 다중 연결용 스캔 함수 - 모든 타겟 디바이스를 리스트로 수집
// device_name: 디바이스 이름 (예: "Tailing")
// mac_addresses: MAC 주소 배열 (NULL 가능, 각 MAC 주소는 18바이트: "AA:BB:CC:DD:EE:FF\0")
// mac_count: MAC 주소 배열의 개수 (0이면 이름만으로 필터링)
// 반환값: 발견된 디바이스 개수
// 동작:
//  - mac_count == 0: device_name만으로 필터링
//  - mac_count > 0: device_name 또는 mac_addresses 배열에 있는 MAC 주소 중 하나라도 일치하면 스캔
int ble_device_scan_multiple(const char* device_name, char mac_addresses[][18], int mac_count);

// 다중 연결 함수 - 스캔된 모든 디바이스에 연결 시도
// 반환값: 연결 성공한 디바이스 개수
int ble_device_connect_multiple(void);

// BLE 연결 해제 및 초기화 해제
void ble_device_disconnect(void);

// 특정 MAC 주소를 가진 BLE 디바이스 연결 해제
// mac_address: 연결 해제할 디바이스의 MAC 주소 ("AA:BB:CC:DD:EE:FF" 형식)
// 반환값: 0 (성공), -1 (실패 또는 디바이스를 찾지 못함)
int ble_device_disconnect_by_mac(const char* mac_address);

// BLE로 받은 데이터 출력
void ble_device_print(void);

// 연결된 디바이스 정보 출력
void ble_info_print(void);

// 현재 활성 BLE 연결 수 조회
int ble_device_get_active_connections(void);

// Notify 데이터 수신 콜백 (필요 시 사용자 구현 가능)
// 기본 구현은 로그 출력만 수행
void ble_device_on_notify(const uint8_t* data, uint16_t length);

// 다중 연결 지원 Notify 콜백 (MAC 주소 파라미터 포함)
// 기본 구현은 MAC 주소로 구분하여 device_collector에 데이터 추가
void ble_device_on_notify_with_mac(const uint8_t* mac_address, const uint8_t* data, uint16_t length);

// BLE 클라이언트로 데이터 전송 (모든 연결된 디바이스에 전송)
// data: 전송할 데이터
// length: 데이터 길이
// 반환값: 전송 성공한 디바이스 개수
int ble_device_send_data(const uint8_t* data, uint16_t length);

// BLE 클라이언트로 문자열 전송 (모든 연결된 디바이스에 전송)
// message: 전송할 문자열 (NULL 종료)
// 반환값: 전송 성공한 디바이스 개수
int ble_device_send_message(const char* message);

// 특정 MAC 주소를 가진 BLE 디바이스에 문자열 전송
// mac_address: 전송할 디바이스의 MAC 주소 ("AA:BB:CC:DD:EE:FF" 형식)
// message: 전송할 문자열 (NULL 종료)
// 반환값: 1 (전송 성공), 0 (전송 실패 또는 디바이스를 찾지 못함)
int ble_target_device_send_message(const char* mac_address, const char* message);

// 연결된 BLE 디바이스의 MAC 주소를 가져오는 함수
// mac_addresses: MAC 주소를 저장할 배열 (각 MAC 주소는 18바이트: "AA:BB:CC:DD:EE:FF\0")
// max_count: 배열의 최대 크기
// 반환값: 실제 연결된 디바이스 개수
int ble_device_get_connected_mac_addresses(char mac_addresses[][18], int max_count);

#endif // BLE_DEVICE_H
