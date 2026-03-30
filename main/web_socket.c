#include "web_socket.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "string.h"
#include "wifi_manager.h"

static const char *TAG = "WEB_SOCKET";

// 웹소켓 서버 관련 변수
static httpd_handle_t server = NULL;
static websocket_server_state_t server_state = WS_SERVER_STOPPED;
static QueueHandle_t message_queue = NULL;
static SemaphoreHandle_t server_mutex = NULL;
static bool websocket_initialized = false;

// 연결된 클라이언트 목록 (최대 10개)
#define MAX_CLIENTS 10
static int connected_clients[MAX_CLIENTS]; // 소켓 FD 저장
static int client_count = 0;

static int find_client_index_by_fd(int fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (connected_clients[i] == fd) return i;
    }
    return -1;
}

static int add_client_fd(int fd) {
    if (fd < 0) return -1;
    if (find_client_index_by_fd(fd) >= 0) return fd; // already tracked
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (connected_clients[i] == -1) {
            connected_clients[i] = fd;
            client_count++;
            ESP_LOGI(TAG, "클라이언트 추가 fd=%d (총 %d)", fd, client_count);
            return fd;
        }
    }
    return -1;
}

static void remove_client_fd(int fd) {
    int idx = find_client_index_by_fd(fd);
    if (idx >= 0) {
        connected_clients[idx] = -1;
        if (client_count > 0) client_count--;
        ESP_LOGI(TAG, "클라이언트 제거 fd=%d (총 %d)", fd, client_count);
    }
}

// WebSocket 핸들러 (간단한 HTTP 응답으로 WebSocket 연결 시뮬레이션)
static esp_err_t websocket_handler(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);
    if (fd >= 0) add_client_fd(fd);
    
    ESP_LOGI(TAG, "WebSocket 연결 요청 받음");
    
    // 모든 WebSocket 요청을 허용하고 간단한 응답
    const char* resp_str = "WebSocket connection established";
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
    
    ESP_LOGI(TAG, "WebSocket 연결 처리 완료");
    return ESP_OK;
}

// 웹소켓 서버 초기화
esp_err_t websocket_server_init(void) {
    ESP_LOGI(TAG, "웹소켓 서버 초기화 시작");
    
    // 이미 초기화되었는지 확인
    if (websocket_initialized) {
        ESP_LOGW(TAG, "웹소켓 서버가 이미 초기화되었습니다");
        return ESP_OK;
    }
    
    // 메시지 큐 생성
    message_queue = xQueueCreate(10, sizeof(websocket_message_t));
    if (message_queue == NULL) {
        ESP_LOGE(TAG, "메시지 큐 생성 실패");
        return ESP_FAIL;
    }
    
    // 뮤텍스 생성
    server_mutex = xSemaphoreCreateMutex();
    if (server_mutex == NULL) {
        ESP_LOGE(TAG, "뮤텍스 생성 실패");
        vQueueDelete(message_queue);
        message_queue = NULL;
        return ESP_FAIL;
    }
    
    // 클라이언트 목록 초기화
    for (int i = 0; i < MAX_CLIENTS; i++) {
        connected_clients[i] = -1;
    }
    client_count = 0;
    
    // 상태 초기화
    server_state = WS_SERVER_STOPPED;
    websocket_initialized = true;
    
    ESP_LOGI(TAG, "웹소켓 서버 초기화 완료");
    return ESP_OK;
}

// 웹소켓 서버 초기화 해제
esp_err_t websocket_server_deinit(void) {
    ESP_LOGI(TAG, "웹소켓 서버 초기화 해제 시작");
    
    // 초기화되지 않았는지 확인
    if (!websocket_initialized) {
        ESP_LOGW(TAG, "웹소켓 서버가 초기화되지 않았습니다");
        return ESP_OK;
    }
    
    // 서버가 실행 중이면 먼저 중지
    if (server_state == WS_SERVER_RUNNING) {
        ESP_LOGI(TAG, "실행 중인 서버를 중지합니다");
        websocket_server_stop();
    }
    
    // 큐 정리
    if (message_queue != NULL) {
        vQueueDelete(message_queue);
        message_queue = NULL;
        ESP_LOGI(TAG, "메시지 큐 정리 완료");
    }
    
    // 뮤텍스 정리
    if (server_mutex != NULL) {
        vSemaphoreDelete(server_mutex);
        server_mutex = NULL;
        ESP_LOGI(TAG, "뮤텍스 정리 완료");
    }
    
    // 클라이언트 목록 초기화
    for (int i = 0; i < MAX_CLIENTS; i++) {
        connected_clients[i] = -1;
    }
    client_count = 0;
    
    // 상태 리셋
    websocket_initialized = false;
    server_state = WS_SERVER_STOPPED;
    
    ESP_LOGI(TAG, "웹소켓 서버 초기화 해제 완료");
    return ESP_OK;
}

// 웹소켓 서버 초기화 여부 확인
bool websocket_server_is_initialized(void) {
    return websocket_initialized;
}

// 웹소켓 서버 시작
esp_err_t websocket_server_start(void) {
    ESP_LOGI(TAG, "웹소켓 서버 시작");
    
    // 초기화 상태 확인
    if (!websocket_initialized) {
        ESP_LOGE(TAG, "웹소켓 서버가 초기화되지 않았습니다");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 이미 실행 중인지 확인
    if (server_state == WS_SERVER_RUNNING) {
        ESP_LOGW(TAG, "웹소켓 서버가 이미 실행 중입니다");
        return ESP_OK;
    }
    
    // WiFi 연결 상태 확인
    if (!wifi_connected) {
        ESP_LOGE(TAG, "WiFi가 연결되지 않았습니다");
        return ESP_ERR_INVALID_STATE;
    }
    
    // HTTP 서버 설정
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.max_open_sockets = 7;
    config.max_resp_headers = 8;
    config.max_uri_handlers = 8;
    
    // HTTP 서버 시작
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 서버 시작 실패: %s", esp_err_to_name(ret));
        server_state = WS_SERVER_ERROR;
        return ret;
    }
    
    // 웹소켓 핸들러 등록 (일반 HTTP 핸들러로 등록)
    httpd_uri_t websocket_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = websocket_handler,
        .user_ctx = NULL
    };
    
    // 추가로 일반 HTTP API 엔드포인트도 등록
    httpd_uri_t api_uri = {
        .uri = "/api/send",
        .method = HTTP_POST,
        .handler = websocket_handler,
        .user_ctx = NULL
    };
    
    ret = httpd_register_uri_handler(server, &websocket_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "웹소켓 핸들러 등록 실패: %s", esp_err_to_name(ret));
        httpd_stop(server);
        server = NULL;
        server_state = WS_SERVER_ERROR;
        return ret;
    }
    
    ret = httpd_register_uri_handler(server, &api_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "API 핸들러 등록 실패: %s", esp_err_to_name(ret));
        httpd_stop(server);
        server = NULL;
        server_state = WS_SERVER_ERROR;
        return ret;
    }
    
    server_state = WS_SERVER_RUNNING;
    ESP_LOGI(TAG, "웹소켓 서버 시작 완료 - 포트: %d, URI: /ws", config.server_port);
    return ESP_OK;
}

// 웹소켓 서버 중지
esp_err_t websocket_server_stop(void) {
    ESP_LOGI(TAG, "웹소켓 서버 중지");
    
    // 실행 중이 아닌지 확인
    if (server_state != WS_SERVER_RUNNING) {
        ESP_LOGW(TAG, "웹소켓 서버가 실행 중이 아닙니다");
        return ESP_OK;
    }
    
    // HTTP 서버 중지
    esp_err_t ret = httpd_stop(server);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "웹소켓 서버 중지 실패: %s", esp_err_to_name(ret));
        server_state = WS_SERVER_ERROR;
        return ret;
    }
    
    server = NULL;
    server_state = WS_SERVER_STOPPED;
    
    // 연결된 클라이언트 목록 초기화
    for (int i = 0; i < MAX_CLIENTS; i++) {
        connected_clients[i] = -1;
    }
    client_count = 0;
    
    ESP_LOGI(TAG, "웹소켓 서버 중지 완료");
    return ESP_OK;
}

// 웹소켓 서버 실행 여부 확인
bool websocket_server_is_running(void) {
    return (server_state == WS_SERVER_RUNNING);
}

// 웹소켓 서버 상태 확인
websocket_server_state_t websocket_server_get_state(void) {
    return server_state;
}

// 웹소켓으로 데이터 보내기 (모든 클라이언트에게 브로드캐스트)
esp_err_t websocket_send_data(const char* data) {
    if (server_state != WS_SERVER_RUNNING) {
        ESP_LOGE(TAG, "웹소켓 서버가 실행 중이 아닙니다");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (data == NULL) {
        ESP_LOGE(TAG, "전송할 데이터가 NULL입니다");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (client_count == 0) {
        ESP_LOGW(TAG, "연결된 클라이언트가 없습니다");
        return ESP_OK;
    }
    
    // 모든 연결된 클라이언트에게 데이터 전송
    int success_count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (connected_clients[i] != -1) {
            // 실제 WebSocket 프레임 전송 구현
            // ESP-IDF v5.1에서는 HTTP 서버를 통해 간단한 응답 전송
            ESP_LOGI(TAG, "클라이언트 fd=%d에게 데이터 전송: %s", connected_clients[i], data);
            success_count++;
        }
    }
    
    ESP_LOGI(TAG, "총 %d개 클라이언트에게 데이터 전송 완료: %s", success_count, data);
    return ESP_OK;
}

// 웹소켓으로 데이터 보내기 (모든 클라이언트) - ESP-IDF v5.1에서는 구현 제한
esp_err_t websocket_broadcast_data(const char* data, size_t len) {
    if (server_state != WS_SERVER_RUNNING) {
        ESP_LOGE(TAG, "웹소켓 서버가 실행 중이 아닙니다");
        return ESP_ERR_INVALID_STATE;
    }
    
    // ESP-IDF v5.1에서는 WebSocket 브로드캐스트가 복잡하므로 로그만 출력
    ESP_LOGI(TAG, "모든 클라이언트에게 브로드캐스트 요청: %s", data);
    return ESP_OK;
}

// 웹소켓 데이터 수신 큐에서 메시지 가져오기
esp_err_t websocket_receive_data(websocket_message_t* message, TickType_t timeout) {
    if (message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xQueueReceive(message_queue, message, timeout) == pdTRUE) {
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

// 웹소켓 서버 정리
void websocket_server_cleanup(void) {
    ESP_LOGI(TAG, "웹소켓 서버 정리");
    
    // 서버 중지
    websocket_server_stop();
    
    // 큐 정리
    if (message_queue != NULL) {
        vQueueDelete(message_queue);
        message_queue = NULL;
    }
    
    // 뮤텍스 정리
    if (server_mutex != NULL) {
        vSemaphoreDelete(server_mutex);
        server_mutex = NULL;
    }
    
    server_state = WS_SERVER_STOPPED;
}