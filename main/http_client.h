#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include "esp_err.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

// 함수 선언
char* send_http_post(const char* path, const char* data);
char* send_http_post_string(const char* path, const char* data);
esp_err_t http_event_handler(esp_http_client_event_t *evt);

// JSON 헬퍼 함수 선언
bool get_json_bool(const char* json_str, const char* key);
char* get_json_string(const char* json_str, const char* key);

#endif // HTTP_CLIENT_H
