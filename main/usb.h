#ifndef USB_H
#define USB_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief USB CDC 초기화
 * 
 * @return esp_err_t 
 *         - ESP_OK: 성공
 *         - ESP_FAIL: 실패
 */
esp_err_t usb_init(void);

/**
 * @brief USB CDC로 문자열 데이터 전송
 * 
 * @param data 전송할 문자열 데이터
 * @return esp_err_t 
 *         - ESP_OK: 성공
 *         - ESP_FAIL: 실패
 */
esp_err_t usb_send_data(const char *data);

/**
 * @brief USB CDC로 포맷 문자열 데이터 전송 (printf 스타일)
 * 
 * @param format 포맷 문자열 (printf 형식)
 * @param ... 가변 인자
 * @return esp_err_t 
 *         - ESP_OK: 성공
 *         - ESP_FAIL: 실패
 */
esp_err_t usb_send_data_fmt(const char *format, ...);

/**
 * @brief USB CDC 연결 상태 확인
 * 
 * @return true 연결됨
 * @return false 연결 안됨
 */
bool usb_is_connected(void);

/**
 * @brief USB CDC 수신 데이터 처리 (메인 루프에서 주기적으로 호출)
 */
void usb_cdc_task(void);

/**
 * @brief USB 수신 데이터 콜백 함수 타입
 * 
 * @param data 수신된 데이터 (널 종료 문자열)
 * @param len 데이터 길이
 */
typedef void (*usb_rx_callback_t)(const char* data, size_t len);

/**
 * @brief USB 수신 데이터 콜백 함수 등록
 * 
 * @param callback 콜백 함수 포인터 (NULL이면 콜백 비활성화)
 */
void usb_set_rx_callback(usb_rx_callback_t callback);

/**
 * @brief 수신된 데이터 가져오기 (콜백 대신 직접 확인)
 * 
 * @param buffer 데이터를 저장할 버퍼
 * @param buffer_size 버퍼 크기
 * @param data_len 수신된 데이터 길이 (NULL 가능)
 * @return true 완전한 메시지가 있음
 * @return false 완전한 메시지가 없음
 */
bool usb_get_received_data(char* buffer, size_t buffer_size, size_t* data_len);

/**
 * @brief USB 통신 수신 준비 (초기화 및 콜백 등록)
 * 
 * USB 초기화가 되어있지 않으면 초기화하고,
 * 콜백이 등록되어 있지 않으면 등록하며,
 * 수신 데이터 처리를 시작합니다.
 * 
 * @param callback 수신 데이터를 처리할 콜백 함수 (NULL이면 콜백만 등록하지 않음)
 * @return esp_err_t 
 *         - ESP_OK: 성공
 *         - ESP_FAIL: 초기화 실패
 */
esp_err_t usb_ready_received(usb_rx_callback_t callback);

/**
 * @brief USB 수신 버퍼 길이 확인 (LED 상태 표시용)
 * 
 * @return size_t 버퍼에 저장된 데이터 길이
 */
size_t usb_get_buffer_len(void);

#endif // USB_H

