#ifndef DEVICE_COLLECTOR_H
#define DEVICE_COLLECTOR_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// 디바이스별 데이터 수집 구조체
typedef struct {
    uint8_t mac_address[6];        // MAC 주소
    char* device_data[250];         // 센서 데이터 배열 (IR,RED,GREEN) 250개
    int data_count;                 // 현재 수집된 데이터 개수

    // 메타데이터: 쉼표 4개(필드 5개) — HR, Spo2, Temp, Battery, Gyro (Temp만 소수)
    int hr;
    int spo2;
    float temp;
    int battery;
    int gyro;

    // 타임스탬프 (첫 데이터 수신 시각)
    char start_time[16];            // HHmmssSSS 형식 (여유 공간 포함)
    bool has_start_time;

    bool is_active;                 // 활성 상태 플래그
    bool overflow_warned;            // 250개 초과 경고를 이미 출력했는지 (로그 스팸 방지)
} device_collector_t;

// 디바이스 수집기 초기화
esp_err_t device_collector_init(void);

// 연결된 디바이스에 대한 객체 생성
// mac_address: 6바이트 MAC 주소 배열
// 반환값: 성공 시 ESP_OK, 실패 시 ESP_ERR_*
esp_err_t device_collector_create_device(const uint8_t* mac_address);

// 특정 디바이스에 데이터 추가
// mac_address: 6바이트 MAC 주소 배열
// data: 수신한 데이터 문자열 (null-terminated)
// 반환값: 성공 시 ESP_OK, 50개 모였으면 ESP_OK (내부에서 처리), 실패 시 ESP_ERR_*
esp_err_t device_collector_add_data(const uint8_t* mac_address, const char* data);

// 디바이스 객체 제거 (연결 해제 시)
// mac_address: 6바이트 MAC 주소 배열
esp_err_t device_collector_remove_device(const uint8_t* mac_address);

// MAC 주소를 문자열로 변환 (디버깅용)
// mac_address: 6바이트 MAC 주소 배열
// buffer: 출력 버퍼 (최소 18바이트)
void device_collector_mac_to_string(const uint8_t* mac_address, char* buffer);

#endif // DEVICE_COLLECTOR_H

