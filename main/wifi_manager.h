#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_gatts_api.h"

// WiFi 관련 상수
#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASSWORD_MAX_LEN 64
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
/** DHCP로 IP를 받은 뒤 connect_to_wifi()에서 대기할 때만 사용 */
#define WIFI_GOT_IP_BIT    BIT2

// NVS 키
#define NVS_WIFI_ID_KEY "nvs_wifi_id"
#define NVS_WIFI_PW_KEY "nvs_wifi_pw"
#define NVS_USER_EMAIL_KEY "nvs_user_email"

// 전역 변수 선언
extern char wifi_id[64];
extern char wifi_pw[64];
extern char user_email[128];
extern char mac_address[18];
extern char response_message[128];
extern EventGroupHandle_t s_wifi_event_group;
extern int s_retry_num;
extern bool wifi_connected;
extern bool wifi_scan_done;
extern bool target_wifi_found;

// 함수 선언
/** esp_wifi_start 전에 반드시 호출 — wifi_init_sta()가 전역 wifi_id/wifi_pw로 설정함 */
void wifi_set_sta_credentials(const char* ssid, const char* password);
/** DHCP/ IP 조회 시 항상 이 핸들 사용 (중복 STA netif 방지) */
esp_netif_t *wifi_get_sta_netif(void);
void wifi_init_sta(void);
esp_err_t wifi_init(void);
bool is_wifi_initialized(void);
void wifi_scan_and_connect(esp_gatt_if_t gatts_if, uint16_t conn_id);
bool connect_to_wifi(const char* ssid, const char* password, char* ip_address);
void send_wifi_success(esp_gatt_if_t gatts_if, uint16_t conn_id);
void send_wifi_fail(esp_gatt_if_t gatts_if, uint16_t conn_id);
bool check_wifi_credentials_exist(void);
bool parse_wifi_command(const char* data, int len);
void send_ok_response(esp_gatt_if_t gatts_if, uint16_t conn_id);
void send_wifi_info(esp_gatt_if_t gatts_if, uint16_t conn_id);
void load_wifi_from_nvs(void);
void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
void wifi_scan_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

#endif // WIFI_MANAGER_H
