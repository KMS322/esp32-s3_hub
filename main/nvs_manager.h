#ifndef NVS_MANAGER_H
#define NVS_MANAGER_H

#include "esp_err.h"
#include <stddef.h>

// NVS 변수 타입 정의
typedef enum {
    NVS_WIFI_ID,
    NVS_WIFI_PW,
    NVS_USER_EMAIL,
    NVS_MAC_ADDRESS
} nvs_var_t;

// NVS 초기화
esp_err_t nvs_init(void);

// NVS에서 값 로드
char* load_nvs(nvs_var_t var);

// NVS에 값 저장
esp_err_t save_nvs(nvs_var_t var, const char* value);

// NVS에서 값 삭제 (초기화)
esp_err_t delete_nvs(nvs_var_t var);

// NVS에서 모든 값 한번에 로드 (구조분해할당 스타일)
esp_err_t load_all_nvs(char** wifi_id, char** wifi_pw, char** user_email, char** mac_address);

// NVS에 모든 값 한번에 저장
esp_err_t save_all_nvs(const char* wifi_id, const char* wifi_pw, const char* user_email, const char* mac_address);

#endif // NVS_MANAGER_H
