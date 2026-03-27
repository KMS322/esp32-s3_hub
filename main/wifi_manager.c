#include "wifi_manager.h"
#include "ble_gatt.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "string.h"
#include "esp_bt.h"
#include "lwip/inet.h"

static const char *TAG = "WIFI_MANAGER";
static esp_netif_t *s_sta_netif = NULL;
static bool s_esp_wifi_inited = false;

// 전역 변수 정의
char wifi_id[64] = {0};
char wifi_pw[64] = {0};
char user_email[128] = {0};
extern char mac_address[18];
char response_message[128] = {0};
EventGroupHandle_t s_wifi_event_group = NULL;
// s_retry_num 제거됨 - main.c에서 재시도 통제
bool wifi_connected = false;
bool wifi_scan_done = false;
bool target_wifi_found = false;

esp_netif_t *wifi_get_sta_netif(void)
{
    if (s_sta_netif != NULL) {
        return s_sta_netif;
    }
    return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

void wifi_set_sta_credentials(const char* ssid, const char* password)
{
    memset(wifi_id, 0, sizeof(wifi_id));
    memset(wifi_pw, 0, sizeof(wifi_pw));
    if (ssid && strlen(ssid) > 0) {
        strncpy(wifi_id, ssid, sizeof(wifi_id) - 1);
    }
    if (password && strlen(password) > 0) {
        strncpy(wifi_pw, password, sizeof(wifi_pw) - 1);
    }
}

// WiFi 이벤트 핸들러
void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    /* STA_START에서 esp_wifi_connect 금지: wifi_init_sta/connect_to_wifi에서만 연결 —
     * 그렇지 않으면 자격 증명 복사 전 빈 SSID로 먼저 연결 시도가 반복됨 */
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* no-op */
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
        
        // main.c에서 재시도를 통제하므로 자동 재시도 제거
        ESP_LOGW(TAG, "WiFi 연결 끊어짐 (reason=%d). main.c에서 재시도 처리합니다.",
                 disconnected ? disconnected->reason : -1);
        
        // EventGroup이 유효한지 확인 후 설정
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        } else {
            ESP_LOGW(TAG, "WiFi EventGroup이 NULL입니다. 생성 중...");
            s_wifi_event_group = xEventGroupCreate();
            if (s_wifi_event_group != NULL) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        }
        wifi_connected = false;
        if (s_wifi_event_group != NULL) {
            xEventGroupClearBits(s_wifi_event_group, WIFI_GOT_IP_BIT);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "WiFi 연결됨 - 이벤트 핸들러 호출됨");
        // EventGroup이 유효한지 확인 후 설정
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        } else {
            ESP_LOGW(TAG, "WiFi EventGroup이 NULL입니다. 생성 중...");
            s_wifi_event_group = xEventGroupCreate();
            if (s_wifi_event_group != NULL) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            }
        }
        wifi_connected = true;
        ESP_LOGI(TAG, "wifi_connected = true 설정됨");

        /* 재연결 후 DHCP가 올라오지 않는 경우 대비 — 실제 바인딩된 STA netif에만 적용 */
        esp_netif_t *sta = wifi_get_sta_netif();
        if (sta != NULL) {
            esp_netif_dhcpc_stop(sta);
            esp_err_t dr = esp_netif_dhcpc_start(sta);
            if (dr != ESP_OK && dr != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
                ESP_LOGW(TAG, "연결 후 DHCP 시작: %s", esp_err_to_name(dr));
            }
        } else {
            ESP_LOGW(TAG, "STA netif 없음, DHCP 시작 생략");
        }
        
        // WiFi 연결 성공 시 앱으로 성공 메시지 전송
        if (current_gatts_if != ESP_GATT_IF_NONE) {
            // send_wifi_success(current_gatts_if, current_conn_id);
        } else if (current_gatts_if == ESP_GATT_IF_NONE) {
            // 실제 HTTP는 main.c의 STATE_MAC_ADDRESS_CHECK에서 수행
            ESP_LOGI(TAG, "WiFi 연결 성공 - HTTP 대기(STATE_MAC_ADDRESS_CHECK)");
            // xTaskCreate(send_test_http_post_task, "http_post_task", 8192, NULL, 5, NULL);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        char ip_str[16] = {0};
        inet_ntoa_r(event->ip_info.ip, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "DHCP IP 할당 완료: %s", ip_str);
        // s_retry_num 제거됨 - main.c에서 재시도 통제
        // EventGroup이 유효한지 확인 후 설정
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_GOT_IP_BIT);
        } else {
            ESP_LOGW(TAG, "WiFi EventGroup이 NULL입니다. 생성 중...");
            s_wifi_event_group = xEventGroupCreate();
            if (s_wifi_event_group != NULL) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_GOT_IP_BIT);
            }
        }
        wifi_connected = true;
        
        // WiFi 연결 성공 시 앱으로 성공 메시지 전송
        if (current_gatts_if != ESP_GATT_IF_NONE) {
            // send_wifi_success(current_gatts_if, current_conn_id);
        } else if (current_gatts_if == ESP_GATT_IF_NONE) {
            // 실제 HTTP는 main.c의 STATE_MAC_ADDRESS_CHECK에서 수행
            ESP_LOGI(TAG, "WiFi 연결 성공 - HTTP 대기(STATE_MAC_ADDRESS_CHECK)");
            // xTaskCreate(send_test_http_post_task, "http_post_task", 8192, NULL, 5, NULL);
        }
    }
}

// WiFi 초기화 함수
void wifi_init_sta(void)
{
    // EventGroup이 이미 생성되었는지 확인
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) {
            ESP_LOGE(TAG, "WiFi EventGroup 생성 실패!");
            return;
        }
    }

    // STA netif는 한 번만 생성
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL) {
            ESP_LOGE(TAG, "STA netif 생성 실패");
            return;
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (!s_esp_wifi_inited) {
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        s_esp_wifi_inited = true;
    }

    // 이벤트 핸들러는 app_main에서 등록됨

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,  // 비밀번호 없는 WiFi용
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    
    // 비밀번호가 있으면 보안 임계값을 완화해 WPA/WPA2/WPA3 혼합 AP도 수용
    if (strlen(wifi_pw) > 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
        ESP_LOGI(TAG, "비밀번호 있음 - WPA 이상 허용 모드");
    } else {
        ESP_LOGI(TAG, "비밀번호 없음 - OPEN 모드");
    }
    
    // WiFi 자격 증명 설정
    strncpy((char*)wifi_config.sta.ssid, wifi_id, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, wifi_pw, sizeof(wifi_config.sta.password) - 1);
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );
    /* DHCP 수신 안정화를 위해 STA 절전 비활성화 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    
    // 비블로킹 방식으로 WiFi 연결 시작 (BLE와 충돌 방지)
    esp_wifi_connect();
}

// WiFi 초기화 상태 변수
static bool wifi_initialized = false;

// WiFi 초기화 함수
esp_err_t wifi_init(void) {
    esp_err_t ret;
    
    // 네트워크 인터페이스 초기화
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 이벤트 루프 생성
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // WiFi 이벤트 핸들러 등록
    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi event handler register failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // IP 이벤트 핸들러 등록
    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "IP event handler register failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // WiFi Station 모드 초기화
    wifi_init_sta();
    wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi 초기화 완료");
    return ESP_OK;
}

bool is_wifi_initialized(void) {
    return wifi_initialized;
}

// WiFi 스캔 이벤트 핸들러
void wifi_scan_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        wifi_event_sta_scan_done_t* scan_done = (wifi_event_sta_scan_done_t*) event_data;
        ESP_LOGI(TAG, "발견된 AP 수: %d", scan_done->number);
        
        wifi_scan_done = true;
        
        if (scan_done->number == 0) {
            target_wifi_found = false;
        } else {
            // 스캔 결과 확인
            uint16_t ap_count = scan_done->number;
            wifi_ap_record_t ap_info[ap_count];
            uint16_t ap_count_found = 0;
            
            esp_wifi_scan_get_ap_records(&ap_count_found, ap_info);
            
            ESP_LOGI(TAG, "스캔된 AP 목록:");
            for (int i = 0; i < ap_count_found; i++) {
                const char* auth_str = "UNKNOWN";
                switch(ap_info[i].authmode) {
                    case WIFI_AUTH_OPEN: auth_str = "OPEN"; break;
                    case WIFI_AUTH_WEP: auth_str = "WEP"; break;
                    case WIFI_AUTH_WPA_PSK: auth_str = "WPA_PSK"; break;
                    case WIFI_AUTH_WPA2_PSK: auth_str = "WPA2_PSK"; break;
                    case WIFI_AUTH_WPA_WPA2_PSK: auth_str = "WPA_WPA2_PSK"; break;
                    case WIFI_AUTH_WPA2_ENTERPRISE: auth_str = "WPA2_ENTERPRISE"; break;
                    case WIFI_AUTH_WPA3_PSK: auth_str = "WPA3_PSK"; break;
                    case WIFI_AUTH_WPA2_WPA3_PSK: auth_str = "WPA2_WPA3_PSK"; break;
                    case WIFI_AUTH_WAPI_PSK: auth_str = "WAPI_PSK"; break;
                    case WIFI_AUTH_OWE: auth_str = "OWE"; break;
                    case WIFI_AUTH_WPA3_ENT_192: auth_str = "WPA3_ENT_192"; break;
                    case WIFI_AUTH_WPA_ENTERPRISE: auth_str = "WPA_ENTERPRISE"; break;
                    default: auth_str = "UNKNOWN"; break;
                }
                ESP_LOGI(TAG, "  %d. SSID: '%s', RSSI: %d, Auth: %s", 
                         i+1, ap_info[i].ssid, ap_info[i].rssi, auth_str);
                
                // 목표 WiFi 찾기 (대소문자 구분 없이)
                if (strcasecmp((char*)ap_info[i].ssid, wifi_id) == 0) {
                    ESP_LOGI(TAG, "SSID: %s, RSSI: %d, Auth: %d", 
                             ap_info[i].ssid, ap_info[i].rssi, ap_info[i].authmode);
                    
                    // 인증 모드 확인
                    if (ap_info[i].authmode == WIFI_AUTH_OPEN) {
                        if (strlen(wifi_pw) == 0) {
                            target_wifi_found = true;
                        } else {
                            target_wifi_found = true; // 비밀번호 무시하고 연결 시도
                        }
                    } else {
                        if (strlen(wifi_pw) > 0) {
                            target_wifi_found = true;
                        } else {
                            target_wifi_found = false;
                        }
                    }
                    break;
                }
            }
            
            if (!target_wifi_found) {
            }
        }
        
        // 스캔 완료 후 연결 시도
        if (target_wifi_found) {
            wifi_init_sta();
        } else {
            // if (current_gatts_if != ESP_GATT_IF_NONE) {
            //     send_wifi_fail(current_gatts_if, current_conn_id);
            // }
        }
        
        // 스캔 완료 후 이벤트 핸들러 해제 (메모리 누수 방지)
        // 이벤트 핸들러는 wifi_scan_and_connect에서 등록된 것이므로
        // 여기서는 해제할 수 없음 - wifi_scan_and_connect에서 해제해야 함
    }
}

// WiFi 스캔 및 연결 함수
void wifi_scan_and_connect(esp_gatt_if_t gatts_if, uint16_t conn_id)
{
    
    if (strlen(wifi_id) == 0) {
        ESP_LOGE(TAG, "WiFi ID is empty, cannot connect");
        // send_wifi_fail(gatts_if, conn_id);
        return;
    }
    
    // 현재 연결 정보 저장
    current_gatts_if = gatts_if;
    current_conn_id = conn_id;
    
    // 받은 WiFi 자격 증명 출력
    ESP_LOGI(TAG, "ID: %s", wifi_id);
    ESP_LOGI(TAG, "PW: %s", strlen(wifi_pw) > 0 ? wifi_pw : "(비밀번호 없음)");
    ESP_LOGI(TAG, "===================");
    
    // WiFi 초기화 (이미 초기화된 경우 체크)
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) {
            ESP_LOGE(TAG, "WiFi EventGroup 생성 실패!");
            // send_wifi_fail(gatts_if, conn_id);
            return;
        }
    }
    
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL) {
            ESP_LOGE(TAG, "STA netif 생성 실패");
            return;
        }
    }

    if (!s_esp_wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        s_esp_wifi_inited = true;
    }

    // 스캔 이벤트 핸들러 등록 (임시로만 등록)
    esp_event_handler_instance_t instance_scan_done;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        WIFI_EVENT_SCAN_DONE,
                                                        &wifi_scan_event_handler,
                                                        NULL,
                                                        &instance_scan_done));

    // WiFi 이벤트 핸들러는 이미 app_main에서 등록됨

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // WiFi 초기화 완료 대기
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // 스캔 시작
    wifi_scan_done = false;
    target_wifi_found = false;
    
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,  // 숨겨진 네트워크는 스캔하지 않음
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 200,  // 스캔 시간 증가
                .max = 500
            }
        }
    };
    
    // 스캔 시작 전에 WiFi 상태 확인
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    
    esp_err_t ret = esp_wifi_scan_start(&scan_config, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 스캔 시작 실패: %s", esp_err_to_name(ret));
        // 스캔 실패 시 이벤트 핸들러 해제
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, instance_scan_done);
        // 스캔 실패 시 직접 연결 시도
        wifi_init_sta();
    } else {
        // 스캔 성공 시 이벤트 핸들러는 wifi_scan_event_handler에서 처리됨
        // 스캔 완료 후 이벤트 핸들러 해제
        vTaskDelay(pdMS_TO_TICKS(5000)); // 스캔 완료 대기
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, instance_scan_done);
    }
}


// WiFi 연결 함수
bool connect_to_wifi(const char* ssid, const char* password, char* ip_address)
{
    
    if (strlen(ssid) == 0) {
        ESP_LOGE(TAG, "WiFi ID is empty, cannot connect");
        return false;
    }

    wifi_set_sta_credentials(ssid, password);

    // 받은 WiFi 자격 증명 출력
    ESP_LOGI(TAG, "ID: %s", ssid);
    ESP_LOGI(TAG, "PW: %s", strlen(password) > 0 ? password : "(비밀번호 없음)");
    ESP_LOGI(TAG, "PW 길이: %d", strlen(password));
    ESP_LOGI(TAG, "===================");

    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) {
            ESP_LOGE(TAG, "EventGroup 생성 실패");
            return false;
        }
    }
    xEventGroupClearBits(s_wifi_event_group, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT);

    // WiFi가 이미 초기화되어 있으므로 설정만 변경
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,  // 기본값: 비밀번호 없는 WiFi용
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    
    // 비밀번호가 있으면 보안 임계값을 완화해 WPA/WPA2/WPA3 혼합 AP도 수용
    // (공백, \r, \n 제거 후 체크)
    char trimmed_pw[64];
    strncpy(trimmed_pw, password, sizeof(trimmed_pw) - 1);
    trimmed_pw[sizeof(trimmed_pw) - 1] = '\0';
    
    // 앞뒤 공백, \r, \n 제거
    char *start = trimmed_pw;
    char *end = start + strlen(trimmed_pw) - 1;
    
    // 앞쪽 공백 제거
    while (start <= end && (*start == ' ' || *start == '\r' || *start == '\n' || *start == '\t')) {
        start++;
    }
    
    // 뒤쪽 공백 제거
    while (end >= start && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t')) {
        *end = '\0';
        end--;
    }
    
    if (strlen(start) > 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
        ESP_LOGI(TAG, "비밀번호 있음 - WPA 이상 허용 모드");
        strncpy((char*)wifi_config.sta.password, start, sizeof(wifi_config.sta.password) - 1);
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        ESP_LOGI(TAG, "비밀번호 없음 - OPEN 모드");
        memset(wifi_config.sta.password, 0, sizeof(wifi_config.sta.password));
    }
    
    // WiFi 자격 증명 설정 (SSID만 설정, 비밀번호는 위에서 이미 설정됨)
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);

    // WiFi 설정 적용
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 설정 실패: %s", esp_err_to_name(ret));
        return false;
    }
    
    
    // WiFi 이벤트 핸들러는 이미 app_main에서 등록됨
    
    // 기존 연결이 있으면 먼저 끊기 (BLE 연결 유지를 위해 짧게)
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    /* disconnect 이벤트가 WIFI_FAIL_BIT를 올릴 수 있음 — 실제 연결 시도 직전에 비트 정리 */
    xEventGroupClearBits(s_wifi_event_group, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT);

    // WiFi 연결 시도
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 연결 시작 실패: %s", esp_err_to_name(ret));
        return false;
    }
    /* 재연결 시에도 절전이 켜지지 않도록 보장 */
    esp_wifi_set_ps(WIFI_PS_NONE);
    
    // 연결 결과 대기 (최대 10초)
    int timeout = 0;
    while (timeout < 100) { // 10초 대기 (100 * 100ms)
        // WiFi 상태 직접 확인
        wifi_ap_record_t ap_info;
        esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "WiFi 연결 성공! SSID: %s, RSSI: %d", ap_info.ssid, ap_info.rssi);
            wifi_connected = true; // 전역 변수도 업데이트
            /* DHCP 대기로 블로킹하지 않고 즉시 상태머신으로 복귀.
             * 실제 IP 확인/대기는 main.c의 STATE_MAC_ADDRESS_CHECK가 담당 */
            if (ip_address != NULL) {
                esp_netif_t *netif = wifi_get_sta_netif();
                if (netif != NULL) {
                    esp_netif_ip_info_t ip_info;
                    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
                        inet_ntoa_r(ip_info.ip, ip_address, 16);
                    } else {
                        strcpy(ip_address, "0.0.0.0");
                    }
                } else {
                    strcpy(ip_address, "0.0.0.0");
                }
            }
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // 100ms 대기
        timeout++;
    }
    
    ESP_LOGE(TAG, "WiFi 연결 타임아웃 - SSID 미발견/5GHz 전용/보안모드 불일치 가능성");
    ESP_LOGE(TAG, "ESP32-S3는 2.4GHz만 지원합니다. 공유기 SSID가 2.4GHz인지 확인하세요.");
    return false;
}

// WiFi 연결 성공 메시지 전송
void send_wifi_success(esp_gatt_if_t gatts_if, uint16_t conn_id) {
    const char* success_message = "wifi connected success\n";
    ESP_LOGI(TAG, "앱으로 성공 메시지 전송: %s", success_message);
    
    esp_err_t ret = esp_ble_gatts_send_indicate(gatts_if, conn_id, 
                                                gl_profile_tab[PROFILE_A_APP_ID].char_handle,
                                                strlen(success_message), 
                                                (uint8_t*)success_message, 
                                                false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "메시지 전송 실패: %s", esp_err_to_name(ret));
    }
    
    // 실제 HTTP는 main.c의 STATE_MAC_ADDRESS_CHECK에서 수행
    ESP_LOGI(TAG, "WiFi 연결 성공 - HTTP 대기(STATE_MAC_ADDRESS_CHECK)");
    // send_test_http_post(); // 기존 함수 비활성화
}

// WiFi 연결 실패 메시지 전송
void send_wifi_fail(esp_gatt_if_t gatts_if, uint16_t conn_id) {
    const char* fail_message = "wifi connect fail\n";
    ESP_LOGI(TAG, "앱으로 실패 메시지 전송: %s", fail_message);
    
    // WiFi 연결 실패 시 자격 증명 및 이메일 초기화
    memset(wifi_id, 0, sizeof(wifi_id));
    memset(wifi_pw, 0, sizeof(wifi_pw));
    memset(user_email, 0, sizeof(user_email));
    
    esp_err_t ret = esp_ble_gatts_send_indicate(gatts_if, conn_id, 
                                                gl_profile_tab[PROFILE_A_APP_ID].char_handle,
                                                strlen(fail_message), 
                                                (uint8_t*)fail_message, 
                                                false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "메시지 전송 실패: %s", esp_err_to_name(ret));
    }
}

// WiFi 자격 증명 존재 여부 확인
bool check_wifi_credentials_exist(void) {
    return (strlen(wifi_id) > 0);
}

// Parse WiFi command from BLE data
bool parse_wifi_command(const char* data, int len) {
    // Convert to null-terminated string
    char command[512] = {0};
    int copy_len = (len < 511) ? len : 511;
    memcpy(command, data, copy_len);
    command[copy_len] = '\0';
    
    // Remove leading/trailing whitespace
    char* trimmed = command;
    while (*trimmed == ' ' || *trimmed == '\t' || *trimmed == '\r' || *trimmed == '\n') {
        trimmed++;
    }
    
    /* 현재 형식: "id,pw,email" (쉼표로 구분) */
    // Find first comma (between ID and password)
    char* comma1 = strchr(trimmed, ',');
    if (comma1 != NULL) {
        // Find second comma (between password and email)
        char* comma2 = strchr(comma1 + 1, ',');
        
        if (comma2 != NULL) {
            // Three-part format: id,pw,email
            // Extract WiFi ID (first part)
            int id_len = comma1 - trimmed;
            if (id_len < 64 && id_len > 0) {
                memset(wifi_id, 0, sizeof(wifi_id));
                memcpy(wifi_id, trimmed, id_len);
                wifi_id[id_len] = '\0';
                
                // Extract WiFi Password (second part)
                int pw_len = comma2 - (comma1 + 1);
                if (pw_len < 64) {
                    memset(wifi_pw, 0, sizeof(wifi_pw));
                    if (pw_len > 0) {
                        memcpy(wifi_pw, comma1 + 1, pw_len);
                        wifi_pw[pw_len] = '\0';
                    }
                    
                    // Extract User Email (third part)
                    char* email_start = comma2 + 1;
                    int email_len = strlen(email_start);
                    if (email_len < 128) {
                        memset(user_email, 0, sizeof(user_email));
                        if (email_len > 0) {
                            memcpy(user_email, email_start, email_len);
                            user_email[email_len] = '\0';
                        }
                        
                        ESP_LOGI(TAG, "WiFi 데이터 파싱 성공: ID=%s, PW=%s, Email=%s", wifi_id, wifi_pw, user_email);
                        return true; // Success
                    } else {
                        ESP_LOGE(TAG, "User email too long (max 127 chars)");
                    }
                } else {
                    ESP_LOGE(TAG, "WiFi password too long (max 63 chars)");
                }
            } else {
                ESP_LOGE(TAG, "WiFi ID too long or empty (max 63 chars)");
            }
        } else {
            // Two-part format: id,pw (email은 빈 문자열)
            int id_len = comma1 - trimmed;
            if (id_len < 64 && id_len > 0) {
                memset(wifi_id, 0, sizeof(wifi_id));
                memcpy(wifi_id, trimmed, id_len);
                wifi_id[id_len] = '\0';
                
                // Extract WiFi Password
                int pw_len = strlen(comma1 + 1);
                if (pw_len < 64) {
                    memset(wifi_pw, 0, sizeof(wifi_pw));
                    if (pw_len > 0) {
                        memcpy(wifi_pw, comma1 + 1, pw_len);
                        wifi_pw[pw_len] = '\0';
                    }
                    
                    // Email은 빈 문자열로 설정
                    memset(user_email, 0, sizeof(user_email));
                    
                    ESP_LOGI(TAG, "WiFi 데이터 파싱 성공: ID=%s, PW=%s", wifi_id, wifi_pw);
                    return true; // Success
                } else {
                    ESP_LOGE(TAG, "WiFi password too long (max 63 chars)");
                }
            } else {
                ESP_LOGE(TAG, "WiFi ID too long or empty (max 63 chars)");
            }
        }
    } else {
        ESP_LOGE(TAG, "Comma not found in data (expected format: id,pw,email or id,pw)");
    }
    
    return false; // Failed
}

// Send "ok" response to smartphone
void send_ok_response(esp_gatt_if_t gatts_if, uint16_t conn_id) {
    const char* ok_message = "ok\n";  // Add newline
    ESP_LOGI(TAG, "Attempting to send OK response: '%s'", ok_message);
    
    // Store response in global variable for reading
    strncpy(response_message, ok_message, sizeof(response_message) - 1);
    response_message[sizeof(response_message) - 1] = '\0';
    
    ESP_LOGI(TAG, "Response stored: '%s'", response_message);
    
    // Send "ok" via BLE notify like the test data
    esp_err_t ret = esp_ble_gatts_send_indicate(gatts_if, conn_id, 
                                                gl_profile_tab[PROFILE_A_APP_ID].char_handle,
                                                strlen(ok_message), 
                                                (uint8_t*)ok_message, 
                                                false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send OK via notify: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "OK sent via notify successfully");
    }
}

// Send WiFi information to smartphone
void send_wifi_info(esp_gatt_if_t gatts_if, uint16_t conn_id) {
    char wifi_info[512]; // Increased buffer size for email
    int len = snprintf(wifi_info, sizeof(wifi_info), "id:%s,pw:%s,email:%s,address:%s", 
                      wifi_id, wifi_pw, user_email, mac_address);  // Include email
    
    if (len >= sizeof(wifi_info)) {
        ESP_LOGE(TAG, "WiFi info too long, truncated");
        len = sizeof(wifi_info) - 1;
        wifi_info[len] = '\0';
    }
    
    // Add newline at the end
    if (len < sizeof(wifi_info) - 1) {
        wifi_info[len] = '\n';
        wifi_info[len + 1] = '\0';
        len++;
    }
    
    esp_err_t ret = esp_ble_gatts_send_indicate(gatts_if, conn_id, 
                                                gl_profile_tab[PROFILE_A_APP_ID].char_handle,
                                                len, 
                                                (uint8_t*)wifi_info, 
                                                false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send WiFi info: %s", esp_err_to_name(ret));
    } else {
    }
}


// NVS에서 WiFi 자격 증명 로드
void load_wifi_from_nvs(void)
{
    ESP_LOGI(TAG, "=== load_wifi_from_nvs() 함수 시작 ===");
    
    nvs_handle_t nvs_handle;
    esp_err_t ret;
    
    // NVS 열기
    ESP_LOGI(TAG, "NVS 열기 시도 중...");
    ret = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS 열기 실패: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "NVS 초기화 상태를 확인하세요!");
        return;
    }
    ESP_LOGI(TAG, "NVS 열기 성공!");
    
    // WiFi ID 로드
    size_t required_size = sizeof(wifi_id);
    ret = nvs_get_str(nvs_handle, NVS_WIFI_ID_KEY, wifi_id, &required_size);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS에서 WiFi ID 로드: %s", wifi_id);
    } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "NVS에 WiFi ID 없음");
        memset(wifi_id, 0, sizeof(wifi_id));
    } else {
        ESP_LOGE(TAG, "WiFi ID 로드 실패: %s", esp_err_to_name(ret));
        memset(wifi_id, 0, sizeof(wifi_id));
    }
    
    // WiFi PW 로드
    required_size = sizeof(wifi_pw);
    ret = nvs_get_str(nvs_handle, NVS_WIFI_PW_KEY, wifi_pw, &required_size);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS에서 WiFi PW 로드: %s", strlen(wifi_pw) > 0 ? wifi_pw : "(empty)");
    } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "NVS에 WiFi PW 없음");
        memset(wifi_pw, 0, sizeof(wifi_pw));
    } else {
        ESP_LOGE(TAG, "WiFi PW 로드 실패: %s", esp_err_to_name(ret));
        memset(wifi_pw, 0, sizeof(wifi_pw));
    }
    
    // User Email 로드
    required_size = sizeof(user_email);
    ret = nvs_get_str(nvs_handle, NVS_USER_EMAIL_KEY, user_email, &required_size);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS에서 User Email 로드: %s", strlen(user_email) > 0 ? user_email : "(empty)");
    } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "NVS에 User Email 없음");
        memset(user_email, 0, sizeof(user_email));
    } else {
        ESP_LOGE(TAG, "User Email 로드 실패: %s", esp_err_to_name(ret));
        memset(user_email, 0, sizeof(user_email));
    }
    
    // NVS 닫기
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "=== NVS 로드 완료 ===");
    ESP_LOGI(TAG, "WiFi ID: %s", strlen(wifi_id) > 0 ? wifi_id : "(empty)");
    ESP_LOGI(TAG, "WiFi PW: %s", strlen(wifi_pw) > 0 ? wifi_pw : "(empty)");
    ESP_LOGI(TAG, "User Email: %s", strlen(user_email) > 0 ? user_email : "(empty)");
    ESP_LOGI(TAG, "==================");
}

