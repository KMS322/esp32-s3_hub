#ifndef WEB_SOCKET_H
#define WEB_SOCKET_H

#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// 웹소켓 서버 상태
typedef enum {
    WS_SERVER_STOPPED,
    WS_SERVER_RUNNING,
    WS_SERVER_ERROR
} websocket_server_state_t;

// 웹소켓 메시지 구조체
typedef struct {
    char data[512];
    size_t len;
    int client_id;
} websocket_message_t;

// 웹소켓 서버 초기화
esp_err_t websocket_server_init(void);

// 웹소켓 서버 초기화 해제
esp_err_t websocket_server_deinit(void);

// 웹소켓 서버 초기화 여부 확인
bool websocket_server_is_initialized(void);

// 웹소켓 서버 시작
esp_err_t websocket_server_start(void);

// 웹소켓 서버 중지
esp_err_t websocket_server_stop(void);

// 웹소켓 서버 실행 여부 확인
bool websocket_server_is_running(void);

// 웹소켓으로 데이터 보내기
esp_err_t websocket_send_data(const char* data);

// 웹소켓으로 데이터 보내기 (모든 클라이언트)
esp_err_t websocket_broadcast_data(const char* data, size_t len);

// 웹소켓 데이터 수신 큐에서 메시지 가져오기
esp_err_t websocket_receive_data(websocket_message_t* message, TickType_t timeout);

// 웹소켓 서버 정리
void websocket_server_cleanup(void);

// 웹소켓 서버 상태 확인
websocket_server_state_t websocket_server_get_state(void);

#endif // WEB_SOCKET_H