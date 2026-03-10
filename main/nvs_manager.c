#include "nvs_manager.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "string.h"
#include "stdlib.h"

static const char *TAG = "NVS_MANAGER";

// NVS 키 이름 정의
static const char* nvs_keys[] = {
    "wifi_id",
    "wifi_pw", 
    "user_email",
    "mac_address"
};

// NVS 초기화
esp_err_t nvs_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGI(TAG, "NVS 파티션 지우기 및 재초기화 중...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS 초기화 완료");
    return ESP_OK;
}

// NVS에서 값 로드
char* load_nvs(nvs_var_t var) {
    nvs_handle_t nvs_handle;
    esp_err_t err;
    size_t required_size = 0;
    char* result = NULL;
    
    // NVS 열기
    err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 열기 실패: %s", esp_err_to_name(err));
        return NULL;
    }
    
    // 필요한 크기 확인
    err = nvs_get_str(nvs_handle, nvs_keys[var], NULL, &required_size);
    if (err == ESP_OK && required_size > 0) {
        // 메모리 할당
        result = (char*)malloc(required_size);
        if (result != NULL) {
            // 실제 값 읽기
            err = nvs_get_str(nvs_handle, nvs_keys[var], result, &required_size);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "NVS 값 읽기 실패: %s", esp_err_to_name(err));
                free(result);
                result = NULL;
            } else {
                ESP_LOGI(TAG, "NVS 로드 성공: %s = %s", nvs_keys[var], result);
            }
        } else {
            ESP_LOGE(TAG, "메모리 할당 실패");
        }
    } else {
        ESP_LOGI(TAG, "NVS 키 없음: %s", nvs_keys[var]);
        // 기본값 반환
        result = (char*)malloc(1);
        if (result != NULL) {
            result[0] = '\0';
        }
    }
    
    nvs_close(nvs_handle);
    return result;
}

// NVS에 값 저장
esp_err_t save_nvs(nvs_var_t var, const char* value) {
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    // NVS 열기
    err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 열기 실패: %s", esp_err_to_name(err));
        return err;
    }
    
    // 값 저장
    err = nvs_set_str(nvs_handle, nvs_keys[var], value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "NVS 저장 성공: %s = %s", nvs_keys[var], value);
        } else {
            ESP_LOGE(TAG, "NVS 커밋 실패: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "NVS 저장 실패: %s", esp_err_to_name(err));
    }
    
    nvs_close(nvs_handle);
    return err;
}

// NVS에서 값 삭제 (초기화)
esp_err_t delete_nvs(nvs_var_t var) {
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    // NVS 열기
    err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 열기 실패: %s", esp_err_to_name(err));
        return err;
    }
    
    // 키 삭제
    err = nvs_erase_key(nvs_handle, nvs_keys[var]);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "NVS 삭제 성공: %s", nvs_keys[var]);
        } else {
            ESP_LOGE(TAG, "NVS 커밋 실패: %s", esp_err_to_name(err));
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "NVS 키 없음: %s", nvs_keys[var]);
        err = ESP_OK;  // 키가 없어도 성공으로 처리
    } else {
        ESP_LOGE(TAG, "NVS 삭제 실패: %s", esp_err_to_name(err));
    }
    
    nvs_close(nvs_handle);
    return err;
}

// NVS에서 모든 값 한번에 로드
esp_err_t load_all_nvs(char** wifi_id, char** wifi_pw, char** user_email, char** mac_address) {
    *wifi_id = load_nvs(NVS_WIFI_ID);
    *wifi_pw = load_nvs(NVS_WIFI_PW);
    *user_email = load_nvs(NVS_USER_EMAIL);
    *mac_address = load_nvs(NVS_MAC_ADDRESS);
    
    return ESP_OK;
}

// NVS에 모든 값 한번에 저장
esp_err_t save_all_nvs(const char* wifi_id, const char* wifi_pw, const char* user_email, const char* mac_address) {
    esp_err_t err1 = save_nvs(NVS_WIFI_ID, wifi_id);
    esp_err_t err2 = save_nvs(NVS_WIFI_PW, wifi_pw);
    esp_err_t err3 = save_nvs(NVS_USER_EMAIL, user_email);
    esp_err_t err4 = save_nvs(NVS_MAC_ADDRESS, mac_address);
    
    return (err1 == ESP_OK && err2 == ESP_OK && err3 == ESP_OK && err4 == ESP_OK) ? ESP_OK : ESP_FAIL;
}
