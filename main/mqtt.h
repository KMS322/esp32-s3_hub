#ifndef MQTT_H
#define MQTT_H

#include "esp_err.h"
#include <stdbool.h>

// MQTT 토픽 설정 함수 (MAC 주소 기반)
// mqtt_init() 호출 전에 반드시 호출해야 함
void mqtt_set_topics(const char* mac_address);

// MQTT 초기화 함수
esp_err_t mqtt_init(void);

// MQTT 초기화 여부 확인
bool mqtt_is_initialized(void);

// MQTT 초기화 완료 확인 (별칭 - 사용 편의성)
bool mqtt_is_init(void);

// MQTT 연결 여부 확인
bool mqtt_is_connected(void);

// MQTT 데이터 전송 함수
esp_err_t mqtt_send_data(const char* topic, const char* data);

// MQTT 데이터 수신 처리 함수 (주기적으로 호출하여 메시지 처리)
void mqtt_receive_data(void);

// MQTT 연결 해제
esp_err_t mqtt_deinit(void);

// MQTT 데이터 전송 함수 (실제 데이터 전송용)
// JSON 데이터를 생성하여 MQTT로 전송
// 반환값: ESP_OK (성공), ESP_ERR_INVALID_STATE (MQTT 미연결), ESP_FAIL (전송 실패)
esp_err_t mqtt_data_send(const char* ip_address);

// MQTT 테스트 전송 함수 (모든 로직 통합)
// WiFi 연결 확인, MQTT 초기화/연결 대기, JSON 생성, 데이터 전송을 모두 처리
// 반환값: ESP_OK (성공), ESP_ERR_INVALID_STATE (WiFi 미연결), ESP_FAIL (전송 실패)
esp_err_t mqtt_test_send(const char* ip_address);

// MQTT 연결 준비 상태 확인 및 초기화 (WiFi 확인 포함)
// 반환값: true (준비 완료), false (준비 안됨 - WiFi 미연결 등)
bool mqtt_ensure_ready(void);

// MQTT 연결 정보 가져오기
// 브로커 URL, 포트, 클라이언트 ID 정보를 반환
void mqtt_get_connection_info(char* broker_url, size_t url_len, int* port, char* client_id, size_t id_len);

// MQTT 토픽 정보 가져오기
// 현재 설정된 토픽을 반환
const char* mqtt_get_topic(void);

#endif // MQTT_H

