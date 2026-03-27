#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/inet.h"

// BLE 관련 헤더
#include "ble_gatt.h"
#include "ble_device.h"
// NVS 매니저
#include "nvs_manager.h"
// WiFi 매니저
#include "wifi_manager.h"
// MQTT 클라이언트
#include "mqtt.h"
// HTTP 클라이언트
#include "http_client.h"
// BLE 클라이언트
#include "ble_client.h"
// WebSocket
#include "web_socket.h"
// GPIO 제어
#include "gpio_control.h"
#include "hub_led.h"
// 디바이스 수집기
#include "device_collector.h"
// USB 통신
#include "usb.h"
// JSON 파싱
#include "cJSON.h"
#define GATTS_TAG "GATTS_DEMO"

// BLE 연결 상태 확인용 (이제 ble_gatt.h에서 선언됨)
extern struct gatts_profile_inst gl_profile_tab[];
extern uint16_t descr_value;

// BLE로 받은 WiFi 데이터
extern char ble_wifi_id[64];
extern char ble_wifi_pw[64]; 
extern char ble_user_email[128];

// 로컬 WiFi 데이터 (NVS에서 로드한 데이터)
char* local_wifi_id = NULL;
char* local_wifi_pw = NULL;
char* local_user_email = NULL;
char* local_mac_address = NULL;

// NVS에서 로드한 WiFi 데이터 (전역 변수)
char* wifi_id_from_nvs = NULL;
char* wifi_pw_from_nvs = NULL;
char* user_email_from_nvs = NULL;
char* mac_address_from_nvs = NULL;

// WiFi IP 주소
char wifi_ip_address[16] = {0};

// 디바이스 MAC 주소 저장 배열
#define MAX_DEVICE_COUNT 5
char device_mac_addresses[MAX_DEVICE_COUNT][18] = {0};
char registered_device_mac_addresses[MAX_DEVICE_COUNT][18] = {0};

// MQTT 수신 데이터 저장용 전역 변수 (mqtt.c에서 접근 가능하도록 extern)
char mqtt_received_data[512] = {0};
bool mqtt_data_received = false;

// MQTT ready 메시지 전송 플래그
static bool mqtt_ready_sent = false;

// Blink 명령으로 받은 타겟 디바이스 MAC 주소
static char target_device_mac_address[18] = {0};

// MAC address 초기화 함수
extern void init_mac_address(void);
extern const char* get_mac_address(void);

// BLE 데이터 수신 플래그 관리 함수 (ble_gatt.c에서 정의)
// extern void set_new_ble_data_received(bool received);
// extern bool get_new_ble_data_received(void);

// WiFi 재시도 관련 변수
// static char last_ble_wifi_id[64] = {0}; // 마지막으로 시도한 WiFi ID 저장
// static char last_ble_wifi_pw[64] = {0}; // 마지막으로 시도한 WiFi PW 저장
int retry_count = 0;
const int MAX_RETRIES = 2;

// NVS WiFi 재시도 관련 변수
static int nvs_wifi_retry_count = 0;
const int NVS_WIFI_MAX_RETRIES = 3;

// USB 통신 관련 변수
static bool usb_init_done = false;
static bool usb_connected_message_sent = false;
static char usb_wifi_id[64] = {0};
static char usb_wifi_pw[64] = {0};
static char usb_user_email[128] = {0};

typedef enum {
    STATE_HUB_INIT,
    STATE_NVS_WIFI_EXIST,
    STATE_NVS_WIFI_NOT_EXIST,
    STATE_USB_WAIT_ACCOUNT,
    STATE_WIFI_CONNECT_TRY,
    STATE_MAC_ADDRESS_CHECK,
    STATE_SCAN_DEVICES,
    STATE_MQTT_INIT,
    STATE_MQTT_DATA_RECEIVE,
    STATE_BLINK_DEVICE,
    STATE_START_DEVICE,
    STATE_STOP_DEVICE,
    STATE_DISCONNECT_DEVICE,
    // STATE_MQTT_DATA_SEND,
    // STATE_DEVICE_DATA_RECEIVE,
    // STATE_WEBSOCKET_INIT,
    // STATE_BLE_WAIT_ACCOUNT,
} hub_state_t;
static hub_state_t current_state = STATE_HUB_INIT;

bool send_another_wifi_data = false;

void clean_string(char* str) {
    char* src = str;
    char* dst = str;

    while (*src) {
        // 개행문자, 캐리지리턴, 탭 등 제거
        if (*src != '\n' && *src != '\r' && *src != '\t') {
            *dst++ = *src;
        }
        src++;
    }
    *dst = '\0';
}

// MAC 주소 추출 함수
static bool extract_mac_address(const char* mac_start, char* output, size_t output_size) {
    // 개행 문자 제거
    char* newline = strchr(mac_start, '\n');
    char* cr = strchr(mac_start, '\r');
    char* end = mac_start + strlen(mac_start);
    if (newline != NULL && newline < end) end = newline;
    if (cr != NULL && cr < end) end = cr;

    size_t mac_len = end - mac_start;
    if (mac_len < output_size) {
        memset(output, 0, output_size);
        strncpy(output, mac_start, mac_len);
        output[mac_len] = '\0';
        return true;
    }
    return false;
}

uint8_t target_macs[][6] = {
    {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC},  // 첫 번째 MAC
    {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56}   // 두 번째 MAC
};

/** NVS 가득 참/포맷 불일치 시 파티션 지우고 재초기화 (ESP-IDF 권장) */
static esp_err_t hub_nvs_flash_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(GATTS_TAG, "NVS: %s — erase & re-init", esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

void app_main(void)
{
    ESP_LOGI(GATTS_TAG, "=== 허브 시작 ===");
    ESP_ERROR_CHECK(hub_nvs_flash_init());
    ESP_ERROR_CHECK(gpio_control_init());
    hub_led_init(HUB_LED_DEFAULT_BRIGHTNESS);
    // 디바이스 수집기 초기화
    ESP_ERROR_CHECK(device_collector_init());
    
    // delete_nvs(NVS_WIFI_ID);
    // delete_nvs(NVS_WIFI_PW);
    // delete_nvs(NVS_USER_EMAIL);
    // delete_nvs(NVS_MAC_ADDRESS);
    // save_nvs(NVS_WIFI_ID, "kms");
    // save_nvs(NVS_WIFI_PW, "12344321");
    // save_nvs(NVS_USER_EMAIL, "n@n.com");

    // init_mac_address();
    // const char* local_mac_address = get_mac_address();
    // ESP_LOGI(GATTS_TAG, "Local MAC Address: %s", local_mac_address);

    while(1) {
        switch(current_state) {
            case STATE_HUB_INIT:
                ESP_LOGI(GATTS_TAG, "###### STATE_HUB_INIT 진입 ######");
                ESP_ERROR_CHECK(hub_nvs_flash_init());
                        
                wifi_id_from_nvs = load_nvs(NVS_WIFI_ID);

                if(wifi_id_from_nvs && strlen(wifi_id_from_nvs) > 0) {
                    hub_led_set_mode(HUB_LED_MODE_WIFI_CONNECTING);
                    wifi_pw_from_nvs = load_nvs(NVS_WIFI_PW);
                    current_state = STATE_NVS_WIFI_EXIST;
                } else {
                    hub_led_set_mode(HUB_LED_MODE_BOOT_NO_WIFI);
                    current_state = STATE_NVS_WIFI_NOT_EXIST;
                }

                vTaskDelay(pdMS_TO_TICKS(10));
                break;

            case STATE_MQTT_INIT:
                ESP_LOGI(GATTS_TAG, "###### STATE_MQTT_INIT 진입 ######");
                if (mac_address_from_nvs != NULL) {
                    ESP_LOGI(GATTS_TAG, "Local MAC Address: %s", mac_address_from_nvs);
                }

                hub_led_set_mode(HUB_LED_MODE_MQTT_CONNECTING);
                
                // WiFi 연결 상태 확인
                if (!wifi_connected) {
                    ESP_LOGW(GATTS_TAG, "WiFi 미연결 - MQTT 초기화 대기");
                    current_state = STATE_NVS_WIFI_EXIST;
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    break;
                }
                
                // MQTT 초기화
                esp_err_t ret = mqtt_init();
                if (ret != ESP_OK) {
                    ESP_LOGE(GATTS_TAG, "MQTT 초기화 실패: %s", esp_err_to_name(ret));
                    hub_led_set_error(HUB_LED_ERR_MQTT_INIT);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    break;
                }
                
                // MQTT 연결 대기 (최대 10초)
                static int init_wait_count = 0;
                if (!mqtt_is_connected()) {
                    init_wait_count++;
                    if (init_wait_count < 10) {
                        ESP_LOGI(GATTS_TAG, "MQTT 연결 대기 중... (%d/10)", init_wait_count);
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        break;
                    } else {
                        ESP_LOGE(GATTS_TAG, "MQTT 연결 시간 초과");
                        hub_led_set_error(HUB_LED_ERR_MQTT_CONNECT);
                        mqtt_deinit();
                        init_wait_count = 0;
                        vTaskDelay(pdMS_TO_TICKS(2000));
                        break;
                    }
                }
                
                // 초기화 완료 - 연결 정보 출력
                init_wait_count = 0;
                ESP_LOGI(GATTS_TAG, "========================================");
                ESP_LOGI(GATTS_TAG, "MQTT 연결 성공!");
                ESP_LOGI(GATTS_TAG, "========================================");
                
                // MQTT 연결 정보 가져오기
                char broker_url[128];
                int broker_port;
                char client_id[64];
                mqtt_get_connection_info(broker_url, sizeof(broker_url), &broker_port, client_id, sizeof(client_id));

                // MQTT ready 메시지 전송 (한 번만)
                if (!mqtt_ready_sent) {
                    const char* topic = mqtt_get_topic();

                    // MAC 주소 포함한 ready 메시지 생성
                    char ready_message[128];
                    if (mac_address_from_nvs != NULL && strlen(mac_address_from_nvs) > 0) {
                        snprintf(ready_message, sizeof(ready_message), "message:%s mqtt ready", mac_address_from_nvs);
                    } else {
                        snprintf(ready_message, sizeof(ready_message), "unknown mqtt ready");
                    }

                    esp_err_t send_ret = mqtt_send_data(topic, ready_message);
                    if (send_ret == ESP_OK) {
                        ESP_LOGI(GATTS_TAG, "MQTT ready 메시지 전송 성공: %s", ready_message);
                        mqtt_ready_sent = true;
                    } else {
                        ESP_LOGW(GATTS_TAG, "MQTT ready 메시지 전송 실패: %s", esp_err_to_name(send_ret));
                        hub_led_set_error(HUB_LED_ERR_MQTT_PUBLISH);
                    }
                }

                if (mqtt_ready_sent) {
                    hub_led_set_mode(HUB_LED_MODE_ONLINE);
                }
                current_state = STATE_MQTT_DATA_RECEIVE;
                break;
            
            case STATE_MQTT_DATA_RECEIVE:
                // MQTT 데이터 수신 대기
                if (mqtt_data_received) {
                    // ESP_LOGI(GATTS_TAG, "MQTT 데이터 수신: %s", mqtt_received_data);
                    
                    // "connect:devices" 또는 "blink:mac_address" 형식 파싱
                    char* colon_pos = strchr(mqtt_received_data, ':');
                    if (colon_pos != NULL) {
                        int cmd_len = colon_pos - mqtt_received_data;
                        char cmd[64] = {0};
                        strncpy(cmd, mqtt_received_data, cmd_len < 63 ? cmd_len : 63);
                        
                        if (strcmp(cmd, "connect") == 0) {
                            ESP_LOGI(GATTS_TAG, "connect 명령 수신 - 디바이스 스캔 시작");
                            // device_mac_addresses 배열 초기화
                            memset(device_mac_addresses, 0, sizeof(device_mac_addresses));
                            // STATE_SCAN_DEVICES로 전환
                            current_state = STATE_SCAN_DEVICES;
                        } else if (strcmp(cmd, "blink") == 0) {
                            if (extract_mac_address(colon_pos + 1, target_device_mac_address,
                                                   sizeof(target_device_mac_address))) {
                                ESP_LOGI(GATTS_TAG, "blink 명령 수신 - 타겟 MAC: %s", target_device_mac_address);
                                current_state = STATE_BLINK_DEVICE;
                            } else {
                                ESP_LOGE(GATTS_TAG, "blink 명령 MAC 주소 길이 오류");
                            }
                        } else if(strcmp(cmd, "start") == 0) {
                            if (extract_mac_address(colon_pos + 1, target_device_mac_address,
                                                   sizeof(target_device_mac_address))) {
                                ESP_LOGI(GATTS_TAG, "start 명령 수신 - 타겟 MAC: %s", target_device_mac_address);
                                current_state = STATE_START_DEVICE;
                            } else {
                                ESP_LOGE(GATTS_TAG, "start 명령 MAC 주소 길이 오류");
                            }
                        } else if(strcmp(cmd, "stop") == 0) {
                            if (extract_mac_address(colon_pos + 1, target_device_mac_address,
                                                   sizeof(target_device_mac_address))) {
                                ESP_LOGI(GATTS_TAG, "stop 명령 수신 - 타겟 MAC: %s", target_device_mac_address);
                                current_state = STATE_STOP_DEVICE;
                            } else {
                                ESP_LOGE(GATTS_TAG, "stop 명령 MAC 주소 길이 오류");
                            }
                        } else if(strcmp(cmd, "disconnect") == 0) {
                            if (extract_mac_address(colon_pos + 1, target_device_mac_address,
                                                   sizeof(target_device_mac_address))) {
                                ESP_LOGI(GATTS_TAG, "disconnect 명령 수신 - 타겟 MAC: %s", target_device_mac_address);
                                current_state = STATE_DISCONNECT_DEVICE;
                            } else {
                                ESP_LOGE(GATTS_TAG, "disconnect 명령 MAC 주소 길이 오류");
                            }
                        } else if(strcmp(cmd, "state") == 0) {
                            // state:hub 명령 수신 - 현재 연결된 디바이스 목록 전송
                            char* param = colon_pos + 1;
                            // 개행 문자 제거
                            char* newline = strchr(param, '\n');
                            if (newline) *newline = '\0';
                            char* cr = strchr(param, '\r');
                            if (cr) *cr = '\0';

                            if (strcmp(param, "hub") == 0) {
                                ESP_LOGI(GATTS_TAG, "state:hub 명령 수신 - 연결된 디바이스 목록 전송");

                                // 실시간으로 연결된 디바이스 MAC 주소 가져오기
                                char connected_macs[MAX_DEVICE_COUNT][18] = {0};
                                int connected_count = ble_device_get_connected_mac_addresses(connected_macs, MAX_DEVICE_COUNT);

                                // device_mac_addresses 배열 업데이트
                                memset(device_mac_addresses, 0, sizeof(device_mac_addresses));
                                for (int i = 0; i < connected_count && i < MAX_DEVICE_COUNT; i++) {
                                    strncpy(device_mac_addresses[i], connected_macs[i], 17);
                                    device_mac_addresses[i][17] = '\0';
                                }

                                // JSON 생성: device:["aa:bb:cc:dd:ee","bb:cc:dd:ee:ff"]
                                cJSON *json_array = cJSON_CreateArray();
                                for (int i = 0; i < connected_count; i++) {
                                    cJSON_AddItemToArray(json_array, cJSON_CreateString(device_mac_addresses[i]));
                                }

                                char *json_string = cJSON_Print(json_array);
                                if (json_string != NULL) {
                                    // "device:" 접두사 추가
                                    char response[512];
                                    snprintf(response, sizeof(response), "device:%s", json_string);

                                    ESP_LOGI(GATTS_TAG, "MQTT 전송: %s", response);

                                    // MQTT로 전송
                                    const char* mqtt_topic = mqtt_get_topic();
                                    if (mqtt_topic != NULL && mqtt_is_connected()) {
                                        esp_err_t mqtt_ret = mqtt_send_data(mqtt_topic, response);
                                        if (mqtt_ret == ESP_OK) {
                                            ESP_LOGI(GATTS_TAG, "state 응답 전송 성공");
                                        } else {
                                            ESP_LOGE(GATTS_TAG, "state 응답 전송 실패");
                                            hub_led_set_error(HUB_LED_ERR_MQTT_PUBLISH);
                                        }
                                    } else {
                                        hub_led_set_error(HUB_LED_ERR_MQTT_CONNECT);
                                    }

                                    free(json_string);
                                } else {
                                    hub_led_set_error(HUB_LED_ERR_MEMORY);
                                }
                                cJSON_Delete(json_array);
                            }
                        }
                    }
                    
                    // 데이터 처리 완료 후 플래그 리셋
                    mqtt_data_received = false;
                    memset(mqtt_received_data, 0, sizeof(mqtt_received_data));
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            
            case STATE_BLINK_DEVICE:
                ESP_LOGI(GATTS_TAG, "###### STATE_BLINK_DEVICE 진입 ######");
                ESP_LOGI(GATTS_TAG, "타겟 디바이스 MAC 주소: %s", target_device_mac_address);

                // MAC 주소가 비어있지 않은지 확인
                if (strlen(target_device_mac_address) > 0) {
                    int result = ble_target_device_send_message(target_device_mac_address, "MODE:D");
                    if (result > 0) {
                        ESP_LOGI(GATTS_TAG, "타겟 디바이스(%s)로 데이터 전송 성공", target_device_mac_address);
                    } else {
                        ESP_LOGW(GATTS_TAG, "타겟 디바이스(%s)로 데이터 전송 실패", target_device_mac_address);
                        hub_led_set_error(HUB_LED_ERR_BLE_SEND);
                    }

                    // 전송 후 MAC 주소 초기화
                    memset(target_device_mac_address, 0, sizeof(target_device_mac_address));
                    ESP_LOGI(GATTS_TAG, "타겟 디바이스 MAC 주소 초기화 완료");
                } else {
                    ESP_LOGW(GATTS_TAG, "타겟 디바이스 MAC 주소가 비어있습니다");
                }

                // MQTT_DATA_RECEIVE 상태로 복귀
                current_state = STATE_MQTT_DATA_RECEIVE;
                vTaskDelay(pdMS_TO_TICKS(10));
                break;
            
            case STATE_START_DEVICE:
                ESP_LOGI(GATTS_TAG, "###### STATE_START_DEVICE 진입 ######");
                ESP_LOGI(GATTS_TAG, "타겟 디바이스 MAC 주소: %s", target_device_mac_address);

                // MAC 주소가 비어있지 않은지 확인
                if (strlen(target_device_mac_address) > 0) {
                    int result = ble_target_device_send_message(target_device_mac_address, "MODE:C");
                    if (result > 0) {
                        ESP_LOGI(GATTS_TAG, "타겟 디바이스(%s)로 데이터 전송 성공", target_device_mac_address);
                    } else {
                        ESP_LOGW(GATTS_TAG, "타겟 디바이스(%s)로 데이터 전송 실패", target_device_mac_address);
                        hub_led_set_error(HUB_LED_ERR_BLE_SEND);
                    }

                    // 전송 후 MAC 주소 초기화
                    memset(target_device_mac_address, 0, sizeof(target_device_mac_address));
                    ESP_LOGI(GATTS_TAG, "타겟 디바이스 MAC 주소 초기화 완료");
                } else {
                    ESP_LOGW(GATTS_TAG, "타겟 디바이스 MAC 주소가 비어있습니다");
                }

                current_state = STATE_MQTT_DATA_RECEIVE;
                vTaskDelay(pdMS_TO_TICKS(10));
                break;

            case STATE_STOP_DEVICE:
                ESP_LOGI(GATTS_TAG, "###### STATE_STOP_DEVICE 진입 ######");
                ESP_LOGI(GATTS_TAG, "타겟 디바이스 MAC 주소: %s", target_device_mac_address);

                // MAC 주소가 비어있지 않은지 확인
                if (strlen(target_device_mac_address) > 0) {
                    int result = ble_target_device_send_message(target_device_mac_address, "MODE:B");
                    if (result > 0) {
                        ESP_LOGI(GATTS_TAG, "타겟 디바이스(%s)로 데이터 전송 성공", target_device_mac_address);
                    } else {
                        ESP_LOGW(GATTS_TAG, "타겟 디바이스(%s)로 데이터 전송 실패", target_device_mac_address);
                        hub_led_set_error(HUB_LED_ERR_BLE_SEND);
                    }

                    // 전송 후 MAC 주소 초기화
                    memset(target_device_mac_address, 0, sizeof(target_device_mac_address));
                    ESP_LOGI(GATTS_TAG, "타겟 디바이스 MAC 주소 초기화 완료");
                } else {
                    ESP_LOGW(GATTS_TAG, "타겟 디바이스 MAC 주소가 비어있습니다");
                }
            
                current_state = STATE_MQTT_DATA_RECEIVE;
                vTaskDelay(pdMS_TO_TICKS(10));
                break;

            case STATE_DISCONNECT_DEVICE:
                ESP_LOGI(GATTS_TAG, "###### STATE_DISCONNECT_DEVICE 진입 ######");
                ESP_LOGI(GATTS_TAG, "타겟 디바이스 MAC 주소: %s", target_device_mac_address);

                // MAC 주소가 비어있지 않은지 확인
                if (strlen(target_device_mac_address) > 0) {
                    // MODE:E 메시지 전송 대신 직접 연결 해제
                    int result = ble_device_disconnect_by_mac(target_device_mac_address);
                    if (result == 0) {
                        ESP_LOGI(GATTS_TAG, "타겟 디바이스(%s) 연결 해제 성공", target_device_mac_address);
                    } else {
                        ESP_LOGW(GATTS_TAG, "타겟 디바이스(%s) 연결 해제 실패", target_device_mac_address);
                        hub_led_set_error(HUB_LED_ERR_BLE_CONNECT);
                    }

                    // 연결 해제 후 MAC 주소 초기화
                    memset(target_device_mac_address, 0, sizeof(target_device_mac_address));
                    ESP_LOGI(GATTS_TAG, "타겟 디바이스 MAC 주소 초기화 완료");
                } else {
                    ESP_LOGW(GATTS_TAG, "타겟 디바이스 MAC 주소가 비어있습니다");
                }

                current_state = STATE_MQTT_DATA_RECEIVE;
                vTaskDelay(pdMS_TO_TICKS(10));
                break;    
  

            // nvs에 wifi id가 존재하는 경우
            case STATE_NVS_WIFI_EXIST:
                ESP_LOGI(GATTS_TAG, "###### STATE_NVS_WIFI_EXIST 진입 ######");
                hub_led_set_mode_if_changed(HUB_LED_MODE_WIFI_CONNECTING);
                ESP_LOGI(GATTS_TAG, "nvs wifi id : %s", wifi_id_from_nvs);
                wifi_pw_from_nvs = load_nvs(NVS_WIFI_PW);
                if(!is_wifi_initialized()) {
                    wifi_set_sta_credentials(wifi_id_from_nvs, wifi_pw_from_nvs);
                    ESP_ERROR_CHECK(wifi_init());
                }
                if(!wifi_connected) {
                    ESP_LOGI(GATTS_TAG, "WiFi 연결 시도 중... (재시도: %d/%d)", nvs_wifi_retry_count, NVS_WIFI_MAX_RETRIES);
                    wifi_connected = connect_to_wifi(wifi_id_from_nvs, wifi_pw_from_nvs, wifi_ip_address);
                    
                    if(wifi_connected) {
                        ESP_LOGI(GATTS_TAG, "WiFi 연결 성공! IP 주소: %s", wifi_ip_address);
                        nvs_wifi_retry_count = 0; // 성공 시 재시도 카운터 리셋
                        current_state = STATE_MAC_ADDRESS_CHECK;
                    } else {
                        nvs_wifi_retry_count++;
                        ESP_LOGW(GATTS_TAG, "WiFi 연결 실패 (재시도: %d/%d)", nvs_wifi_retry_count, NVS_WIFI_MAX_RETRIES);
                        
                        if(nvs_wifi_retry_count >= NVS_WIFI_MAX_RETRIES) {
                            ESP_LOGE(GATTS_TAG, "WiFi 연결 최대 재시도 횟수 도달 - NVS WiFi 정보 삭제 및 BLE 모드로 전환");
                            hub_led_set_error(HUB_LED_ERR_WIFI);
                            vTaskDelay(pdMS_TO_TICKS(6000));
                            // NVS에서 WiFi 정보 삭제
                            delete_nvs(NVS_WIFI_ID);
                            delete_nvs(NVS_WIFI_PW);
                            
                            // WiFi 관련 변수 해제
                            if(wifi_id_from_nvs != NULL) {
                                free(wifi_id_from_nvs);
                                wifi_id_from_nvs = NULL;
                            }
                            if(wifi_pw_from_nvs != NULL) {
                                free(wifi_pw_from_nvs);
                                wifi_pw_from_nvs = NULL;
                            }
                            
                            nvs_wifi_retry_count = 0; // 재시도 카운터 리셋
                            hub_led_set_mode(HUB_LED_MODE_USB_WAIT);
                            current_state = STATE_NVS_WIFI_NOT_EXIST;
                        } else {
                            // 재시도 대기 후 다시 시도
                            vTaskDelay(pdMS_TO_TICKS(2000)); // 2초 대기 후 재시도
                        }
                    }
                } else {
                    // 이미 연결되어 있으면 다음 상태로
                    current_state = STATE_MAC_ADDRESS_CHECK;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                break;

            // nvs에 wifi id가 존재하지 않는 경우
              case STATE_NVS_WIFI_NOT_EXIST: 
                ESP_LOGI(GATTS_TAG, "###### STATE_NVS_WIFI_NOT_EXIST 진입 ######");
                hub_led_set_mode(HUB_LED_MODE_USB_WAIT);
                // if (!is_ble_initialized()) {
                //     ESP_ERROR_CHECK(ble_init());
                //     // ESP_ERROR_CHECK(ble_start_advertising());
                //     ESP_LOGI(GATTS_TAG, "BLE 서버 초기화 및 광고 시작 완료");
                //     ws2812_blink_start_rgb(0, 0, 255);
                // }
                current_state = STATE_USB_WAIT_ACCOUNT;
                vTaskDelay(pdMS_TO_TICKS(10));
                break;

            case STATE_USB_WAIT_ACCOUNT:
                {
                    if (!usb_init_done) {
                        hub_led_set_mode_if_changed(HUB_LED_MODE_USB_WAIT);
                    } else {
                        hub_led_set_mode_if_changed(HUB_LED_MODE_USB_LINE_OK);
                    }
                    // USB 초기화 (한 번만 실행)
                    if (!usb_init_done) {
                        esp_err_t ret = usb_init();
                        if (ret == ESP_OK) {
                            usb_init_done = true;
                            hub_led_set_mode(HUB_LED_MODE_USB_LINE_OK);
                            usb_connected_message_sent = false; // 연결 메시지 전송 플래그 리셋
                        } else {
                            hub_led_set_error(HUB_LED_ERR_USB_CDC);
                            vTaskDelay(pdMS_TO_TICKS(5000));
                        }
                    }
                    
                    // 초기화가 완료된 경우에만 데이터 전송 및 수신 처리
                    if (usb_init_done) {
                        // USB 연결 메시지 한 번만 전송
                        if (!usb_connected_message_sent) {
                            esp_err_t send_ret = usb_send_data("usb connected\n");
                            if (send_ret == ESP_OK) {
                                ESP_LOGI(GATTS_TAG, "USB 연결 메시지 전송 성공");
                                usb_connected_message_sent = true;
                            } else {
                                ESP_LOGE(GATTS_TAG, "USB 연결 메시지 전송 실패: %s", esp_err_to_name(send_ret));
                            }
                        }
                        // USB 수신 데이터 처리 (매번 호출 - 중요!)
                        extern void usb_cdc_task(void);
                        usb_cdc_task();
                        
                        // 데이터 받아오기 (정적 변수로 선언하여 스택 오버플로우 방지)
                        static char received_data[2048];
                        size_t data_len = 0;
                        if (usb_get_received_data(received_data, sizeof(received_data), &data_len)) {
                            // ESP_LOGI(GATTS_TAG, "USB 데이터 수신: %s (길이: %zu)", received_data, data_len);
                            
                            // "wifi"가 포함되어 있는지 확인
                            if (strstr(received_data, "wifi") != NULL) {
                                // "wifi:" 다음 부분 찾기
                                char* wifi_prefix = strstr(received_data, "wifi:");
                                if (wifi_prefix != NULL) {
                                    char* data_start = wifi_prefix + 5; // "wifi:" 다음 위치
                                    
                                    // 첫 번째 쉼표 찾기 (WiFi ID)
                                    char* comma1 = strchr(data_start, ',');
                                    if (comma1 != NULL) {
                                        size_t id_len = comma1 - data_start;
                                        if (id_len < sizeof(usb_wifi_id)) {
                                            memset(usb_wifi_id, 0, sizeof(usb_wifi_id));
                                            strncpy(usb_wifi_id, data_start, id_len);
                                            usb_wifi_id[id_len] = '\0';
                                            
                                            // 두 번째 쉼표 찾기 (WiFi PW)
                                            char* comma2 = strchr(comma1 + 1, ',');
                                            if (comma2 != NULL) {
                                                size_t pw_len = comma2 - (comma1 + 1);
                                                if (pw_len < sizeof(usb_wifi_pw)) {
                                                    memset(usb_wifi_pw, 0, sizeof(usb_wifi_pw));
                                                    strncpy(usb_wifi_pw, comma1 + 1, pw_len);
                                                    usb_wifi_pw[pw_len] = '\0';
                                                    
                                                    // 나머지 부분 (User Email)
                                                    char* email_start = comma2 + 1;
                                                    // 개행 문자 제거
                                                    char* newline = strchr(email_start, '\n');
                                                    char* cr = strchr(email_start, '\r');
                                                    char* end = email_start + strlen(email_start);
                                                    if (newline != NULL && newline < end) end = newline;
                                                    if (cr != NULL && cr < end) end = cr;
                                                    
                                                    size_t email_len = end - email_start;
                                                    if (email_len < sizeof(usb_user_email)) {
                                                        memset(usb_user_email, 0, sizeof(usb_user_email));
                                                        strncpy(usb_user_email, email_start, email_len);
                                                        usb_user_email[email_len] = '\0';
                                                        
                                                        ESP_LOGI(GATTS_TAG, "USB WiFi 데이터 파싱 완료 - ID: %s, PW: %s, Email: %s", 
                                                                 usb_wifi_id, usb_wifi_pw, usb_user_email);
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                                retry_count = 0; // WiFi 연결 시도 전에 재시도 카운터 리셋
                                hub_led_set_mode(HUB_LED_MODE_WIFI_CONNECTING);
                                current_state = STATE_WIFI_CONNECT_TRY;
                                ESP_LOGI(GATTS_TAG, "###### STATE_WIFI_CONNECT_TRY로 case 전환 ######");
                            }
                        }
                        
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                break;    

         

            // ble 또는 usb로 받은 wifi 계정으로 wifi 연결을 시도하는 경우
            // 성공하면 nvs에 저장하고 실패하면 다시 대기 모드로 돌아감
            case STATE_WIFI_CONNECT_TRY:
                ESP_LOGI(GATTS_TAG, "###### STATE_WIFI_CONNECT_TRY 진입 ######");
                hub_led_set_mode_if_changed(HUB_LED_MODE_WIFI_CONNECTING);
                ESP_LOGI(GATTS_TAG, "retry_count: %d", retry_count);
                ESP_LOGI(GATTS_TAG, "MAX_RETRIES: %d", MAX_RETRIES);
                
                // USB 또는 BLE에서 받은 WiFi 정보 사용
                const char* wifi_id = NULL;
                const char* wifi_pw = NULL;
                const char* user_email = NULL;
                bool is_usb_mode = false;
                
                // USB에서 받은 데이터가 있으면 USB 데이터 사용
                if (strlen(usb_wifi_id) > 0) {
                    wifi_id = usb_wifi_id;
                    wifi_pw = usb_wifi_pw;
                    user_email = usb_user_email;
                    is_usb_mode = true;
                    ESP_LOGI(GATTS_TAG, "USB WiFi 데이터 사용: ID=%s, PW=%s, Email=%s", wifi_id, wifi_pw, user_email);
                } else if (strlen(ble_wifi_id) > 0) {
                    wifi_id = ble_wifi_id;
                    wifi_pw = ble_wifi_pw;
                    user_email = ble_user_email;
                    is_usb_mode = false;
                    ESP_LOGI(GATTS_TAG, "BLE WiFi 데이터 사용: ID=%s, PW=%s, Email=%s", wifi_id, wifi_pw, user_email);
                } else {
                    ESP_LOGE(GATTS_TAG, "WiFi 정보가 없습니다");
                    current_state = STATE_USB_WAIT_ACCOUNT;
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    break;
                }
                
                if(retry_count >= MAX_RETRIES) {
                    hub_led_set_error(HUB_LED_ERR_WIFI);
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    if (is_usb_mode) {
                        usb_send_data("wifi connect fail\n");
                        hub_led_set_mode(HUB_LED_MODE_USB_WAIT);
                        current_state = STATE_USB_WAIT_ACCOUNT;
                        ESP_LOGI(GATTS_TAG, "###### STATE_USB_WAIT_ACCOUNT로 case 전환 ######");
                    } else {
                        send_ble_message("wifi connect fail\n");
                        current_state = STATE_HUB_INIT;
                        ESP_LOGI(GATTS_TAG, "###### STATE_HUB_INIT로 case 전환 ######");
                    }
                } else {
                    if(!is_wifi_initialized()) {
                        wifi_set_sta_credentials(wifi_id, wifi_pw);
                        ESP_ERROR_CHECK(wifi_init());
                    }
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    wifi_connected = connect_to_wifi(wifi_id, wifi_pw, wifi_ip_address);
                    if(wifi_connected) {
                        ESP_LOGI(GATTS_TAG, "WiFi IP 주소: %s", wifi_ip_address);
                        if (is_usb_mode) {
                            usb_send_data("wifi connected success\n");
                        } else {
                            send_ble_message("wifi connected success\n");
                            ble_disconnect();
                        }
                        if (save_nvs(NVS_WIFI_ID, wifi_id) != ESP_OK ||
                            save_nvs(NVS_WIFI_PW, wifi_pw) != ESP_OK ||
                            save_nvs(NVS_USER_EMAIL, user_email) != ESP_OK) {
                            hub_led_set_error(HUB_LED_ERR_NVS_SAVE);
                        }
                        init_mac_address();
                        const char* local_mac_address = get_mac_address();
                        if (save_nvs(NVS_MAC_ADDRESS, local_mac_address) != ESP_OK) {
                            hub_led_set_error(HUB_LED_ERR_NVS_SAVE);
                        }
    
                        // USB 변수 초기화
                        memset(usb_wifi_id, 0, sizeof(usb_wifi_id));
                        memset(usb_wifi_pw, 0, sizeof(usb_wifi_pw));
                        memset(usb_user_email, 0, sizeof(usb_user_email));
    
                        current_state = STATE_HUB_INIT;
                    } else {
                        retry_count++;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                break;

            // express 서버에 hub가 등록되었는지 요청하는 경우
            case STATE_MAC_ADDRESS_CHECK:   
                ESP_LOGI(GATTS_TAG, "###### STATE_MAC_ADDRESS_CHECK 진입 ######");
                hub_led_set_mode_if_changed(HUB_LED_MODE_HTTP_CHECKING);
                // NVS 값이 비어있거나 아직 로드되지 않았다면 로드 (NULL 안전)
                if (user_email_from_nvs == NULL || strlen(user_email_from_nvs) == 0) {
                    user_email_from_nvs = load_nvs(NVS_USER_EMAIL);
                }
                if (mac_address_from_nvs == NULL || strlen(mac_address_from_nvs) == 0) {
                    mac_address_from_nvs = load_nvs(NVS_MAC_ADDRESS);
                    // NVS에 MAC 주소가 없으면 현재 MAC 주소를 가져와서 사용
                    if (mac_address_from_nvs == NULL || strlen(mac_address_from_nvs) == 0) {
                        init_mac_address();
                        const char* current_mac = get_mac_address();
                        if (current_mac != NULL && strlen(current_mac) > 0) {
                            // 기존 메모리 해제 후 새로 할당
                            if (mac_address_from_nvs != NULL) {
                                free(mac_address_from_nvs);
                            }
                            mac_address_from_nvs = malloc(strlen(current_mac) + 1);
                            if (mac_address_from_nvs != NULL) {
                                strcpy(mac_address_from_nvs, current_mac);
                                ESP_LOGI(GATTS_TAG, "현재 MAC 주소 사용: %s", mac_address_from_nvs);
                            } else {
                                hub_led_set_error(HUB_LED_ERR_MEMORY);
                            }
                        }
                    }
                }

                if(wifi_connected) {
                    // IP 주소가 할당되었는지 확인
                    if (strcmp(wifi_ip_address, "0.0.0.0") == 0 || strlen(wifi_ip_address) == 0) {
                        ESP_LOGW(GATTS_TAG, "IP 주소가 할당되지 않음 - 재시도 대기");
                        // WiFi IP 주소 다시 확인
                        esp_netif_t *netif = wifi_get_sta_netif();
                        if (netif != NULL) {
                            esp_netif_ip_info_t ip_info;
                            esp_err_t ip_ret = esp_netif_get_ip_info(netif, &ip_info);
                            if (ip_ret == ESP_OK && ip_info.ip.addr != 0) {
                                char ip_str[16];
                                inet_ntoa_r(ip_info.ip, ip_str, sizeof(ip_str));
                                strncpy(wifi_ip_address, ip_str, sizeof(wifi_ip_address) - 1);
                                wifi_ip_address[sizeof(wifi_ip_address) - 1] = '\0';
                                ESP_LOGI(GATTS_TAG, "IP 주소 확인 완료: %s", wifi_ip_address);
                            }
                        }
                        vTaskDelay(pdMS_TO_TICKS(2000)); // 2초 대기 후 재시도
                        break;
                    }
                    
                    char json_data[256];
                    char clean_email[128];
                    char clean_mac[32];
                    
                    // 안전한 복사
                    strncpy(clean_email, (user_email_from_nvs != NULL) ? user_email_from_nvs : "", sizeof(clean_email) - 1);
                    strncpy(clean_mac, (mac_address_from_nvs != NULL) ? mac_address_from_nvs : "", sizeof(clean_mac) - 1);
                    clean_email[sizeof(clean_email) - 1] = '\0';
                    clean_mac[sizeof(clean_mac) - 1] = '\0';
                    
                    // 개행문자 제거
                    clean_string(clean_email);
                    clean_string(clean_mac);
                    
                    snprintf(json_data, sizeof(json_data),
                             "{\"user_email\":\"%s\",\"mac_address\":\"%s\"}",
                             clean_email, clean_mac);
                    
                    ESP_LOGI(GATTS_TAG, "최종 JSON: %s", json_data);
                    char* response = send_http_post("/check/hub", json_data);
                    if (response != NULL) {
                        ESP_LOGI(GATTS_TAG, "서버 응답: %s", response);

                        // "mqtt server ready:aa:bb:cc:dd:ee,bb:cc:dd:ee:ff" 형식 파싱
                        char* mqtt_ready_pos = strstr(response, "mqtt server ready");
                        if (mqtt_ready_pos != NULL) {
                            // registered_device_mac_addresses 배열 초기화
                            memset(registered_device_mac_addresses, 0, sizeof(registered_device_mac_addresses));

                            // 콜론 찾기
                            char* colon_pos = strchr(mqtt_ready_pos, ':');
                            if (colon_pos != NULL) {
                                // 콜론 다음부터 MAC 주소 리스트 시작
                                char* mac_list_start = colon_pos + 1;

                                // 개행 문자 제거
                                char* newline = strchr(mac_list_start, '\n');
                                char* cr = strchr(mac_list_start, '\r');
                                char* end = mac_list_start + strlen(mac_list_start);
                                if (newline != NULL && newline < end) end = newline;
                                if (cr != NULL && cr < end) end = cr;

                                // 쉼표로 구분된 MAC 주소 파싱
                                int mac_index = 0;
                                char* token_start = mac_list_start;
                                char* token_end = NULL;

                                while (token_start < end && mac_index < MAX_DEVICE_COUNT) {
                                    // 앞쪽 공백 제거
                                    while (token_start < end && (*token_start == ' ' || *token_start == '\t')) {
                                        token_start++;
                                    }

                                    // 다음 쉼표 찾기
                                    token_end = strchr(token_start, ',');
                                    if (token_end == NULL || token_end > end) {
                                        token_end = end;
                                    }

                                    // 뒤쪽 공백 제거
                                    char* trimmed_end = token_end;
                                    while (trimmed_end > token_start && (*(trimmed_end - 1) == ' ' || *(trimmed_end - 1) == '\t')) {
                                        trimmed_end--;
                                    }

                                    // MAC 주소 길이 확인
                                    size_t mac_len = trimmed_end - token_start;
                                    if (mac_len > 0 && mac_len < 18) {
                                        strncpy(registered_device_mac_addresses[mac_index], token_start, mac_len);
                                        registered_device_mac_addresses[mac_index][mac_len] = '\0';
                                        ESP_LOGI(GATTS_TAG, "등록된 디바이스 MAC[%d]: %s", mac_index, registered_device_mac_addresses[mac_index]);
                                        mac_index++;
                                    }

                                    // 다음 토큰으로 이동
                                    if (token_end < end) {
                                        token_start = token_end + 1; // 쉼표 다음
                                    } else {
                                        break;
                                    }
                                }

                                ESP_LOGI(GATTS_TAG, "총 %d개의 등록된 디바이스 MAC 주소 파싱 완료", mac_index);
                            }

                            // MQTT 토픽 설정 (MAC 주소 기반)
                            mqtt_set_topics(clean_mac);

                            current_state = STATE_MQTT_INIT;
                        } else {
                            ESP_LOGW(GATTS_TAG, "서버 응답에 mqtt server ready 없음");
                            hub_led_set_error(HUB_LED_ERR_HTTP);
                        }

                        vTaskDelay(pdMS_TO_TICKS(30));
                    } else {
                        ESP_LOGE(GATTS_TAG, "HTTP POST 요청 실패");
                        hub_led_set_error(HUB_LED_ERR_HTTP);
                    }
                } else {
                    wifi_connected = connect_to_wifi(wifi_id_from_nvs, wifi_pw_from_nvs, wifi_ip_address);
                    if(wifi_connected) {
                        ESP_LOGI(GATTS_TAG, "WiFi IP 주소: %s", wifi_ip_address);
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(50));
                break;
                
            case STATE_SCAN_DEVICES :
                ESP_LOGI(GATTS_TAG, "###### STATE_SCAN_DEVICES 진입 ######");

                // BLE 서버가 실행 중이면 완전 종료
                if (is_ble_initialized()) {
                    ESP_LOGI(GATTS_TAG, "BLE 서버 완전 종료 중...");
                    ble_complete_deinit();
                    ESP_LOGI(GATTS_TAG, "BLE 스택 완전 해제 대기 중...");
                    vTaskDelay(pdMS_TO_TICKS(3000)); // 3초 대기로 증가 - BT 스택 완전 해제
                }

                hub_led_set_mode(HUB_LED_MODE_BLE_SCAN);

                // BLE 클라이언트 초기화 상태 확인
                ESP_LOGI(GATTS_TAG, "BLE 클라이언트 초기화 상태: %s", ble_device_is_init_func() ? "초기화됨" : "미초기화");

                if(!ble_device_is_init_func()) {
                    ESP_LOGI(GATTS_TAG, "BLE 클라이언트 초기화 시작...");
                    esp_err_t ret = ble_device_init();
                    if (ret != ESP_OK) {
                        ESP_LOGE(GATTS_TAG, "BLE 클라이언트 초기화 실패: %s", esp_err_to_name(ret));
                        hub_led_set_error(HUB_LED_ERR_BLE_SCAN);
                        vTaskDelay(pdMS_TO_TICKS(5000));
                        break;
                    }
                    // ESP_LOGI(GATTS_TAG, "BLE 클라이언트 초기화 완료");
                    vTaskDelay(pdMS_TO_TICKS(1000)); // 초기화 후 안정화 대기
                }
                // 다중 스캔 및 연결
                // device_mac_addresses 배열의 실제 개수 계산
                int mac_count = 0;
                for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
                    if (strlen(device_mac_addresses[i]) > 0) {
                        mac_count++;
                    }
                }

                int scanned_count = ble_device_scan_multiple("Tailing", device_mac_addresses, mac_count);
                if (scanned_count > 0) {
                    ESP_LOGI(GATTS_TAG, "스캔 완료: %d개 디바이스 발견", scanned_count);
                    
                    // 모든 디바이스에 연결 시도
                    int connected_count = ble_device_connect_multiple();
                    if (connected_count > 0) {
                        ESP_LOGI(GATTS_TAG, "다중 연결 성공: %d개 디바이스 연결됨", connected_count);
                        bool scan_report_mqtt_ok = false;

                        // 연결된 디바이스들의 MAC 주소를 배열에 저장
                        char connected_macs[MAX_DEVICE_COUNT][18] = {0};
                        int mac_count = ble_device_get_connected_mac_addresses(connected_macs, MAX_DEVICE_COUNT);
                        
                        if (mac_count > 0) {
                            // device_mac_addresses 배열에 저장
                            for (int i = 0; i < mac_count && i < MAX_DEVICE_COUNT; i++) {
                                strncpy(device_mac_addresses[i], connected_macs[i], 17);
                                device_mac_addresses[i][17] = '\0';
                                ESP_LOGI(GATTS_TAG, "연결된 디바이스 MAC[%d]: %s", i, device_mac_addresses[i]);
                            }
                            
                            // JSON 생성: {"connected_devices": ["aa:bb:cc:dd:ee", ...]}
                            cJSON *json = cJSON_CreateObject();
                            cJSON *devices_array = cJSON_CreateArray();
                            for (int i = 0; i < mac_count; i++) {
                                cJSON_AddItemToArray(devices_array, cJSON_CreateString(device_mac_addresses[i]));
                            }
                            cJSON_AddItemToObject(json, "connected_devices", devices_array);
                            
                            char *json_string = cJSON_Print(json);
                            if (json_string != NULL) {
                                ESP_LOGI(GATTS_TAG, "MQTT 전송할 JSON: %s", json_string);
                                
                                // MQTT로 전송
                                const char* mqtt_topic = mqtt_get_topic();
                                if (mqtt_topic != NULL && mqtt_is_connected()) {
                                    esp_err_t mqtt_ret = mqtt_send_data(mqtt_topic, json_string);
                                    if (mqtt_ret == ESP_OK) {
                                        ESP_LOGI(GATTS_TAG, "MQTT 전송 성공");
                                        scan_report_mqtt_ok = true;
                                    } else {
                                        ESP_LOGE(GATTS_TAG, "MQTT 전송 실패: %s", esp_err_to_name(mqtt_ret));
                                        hub_led_set_error(HUB_LED_ERR_MQTT_PUBLISH);
                                    }
                                } else {
                                    ESP_LOGE(GATTS_TAG, "MQTT 연결되지 않음");
                                    hub_led_set_error(HUB_LED_ERR_MQTT_CONNECT);
                                }
                                
                                free(json_string);
                            } else {
                                hub_led_set_error(HUB_LED_ERR_MEMORY);
                            }
                            cJSON_Delete(json);
                        }
                        
                        // 다음 상태로 전환: STATE_MQTT_DATA_RECEIVE로 복귀
                        if (scan_report_mqtt_ok) {
                            hub_led_set_mode(HUB_LED_MODE_ONLINE);
                        }
                        current_state = STATE_MQTT_DATA_RECEIVE;
                    } else {
                        ESP_LOGW(GATTS_TAG, "연결 실패 - 재시도");
                        hub_led_set_error(HUB_LED_ERR_BLE_CONNECT);
                    }
                } else {
                    ESP_LOGW(GATTS_TAG, "스캔 실패 또는 디바이스 없음 - 재시도");
                    hub_led_set_error(HUB_LED_ERR_BLE_SCAN);
                }

                vTaskDelay(pdMS_TO_TICKS(500));
                break;

                   // ble로 데이터 받는 것을 대기중인 경우
            // case STATE_BLE_WAIT_ACCOUNT:
            //     // ESP_LOGI(GATTS_TAG, "STATE_BLE_WAIT_ACCOUNT 진입");
            //     if(ble_connected) {
            //         ws2812_blink_stop(); 
            //         ws2812_set_color(0, 0, 255);
            //     }
            //     if (strlen(ble_wifi_id) > 0) {
            //         // 새로운 BLE 데이터가 수신되었는지 확인
            //         if (get_new_ble_data_received()) {
            //             bool is_new_wifi_data = (strlen(last_ble_wifi_id) == 0) ||  // 처음이거나
            //                                    (strcmp(ble_wifi_id, last_ble_wifi_id) != 0) || 
            //                                    (strcmp(ble_wifi_pw, last_ble_wifi_pw) != 0);
            //             if(is_new_wifi_data) {
            //                 // 새로운 데이터 저장
            //                 strncpy(last_ble_wifi_id, ble_wifi_id, sizeof(last_ble_wifi_id) - 1);
            //                 last_ble_wifi_id[sizeof(last_ble_wifi_id) - 1] = '\0';
            //                 strncpy(last_ble_wifi_pw, ble_wifi_pw, sizeof(last_ble_wifi_pw) - 1);
            //                 last_ble_wifi_pw[sizeof(last_ble_wifi_pw) - 1] = '\0';
                            
            //                 retry_count = 0;
            //                 send_another_wifi_data = false;
            //                 set_new_ble_data_received(false); // 플래그 리셋
            //                 current_state = STATE_WIFI_CONNECT_TRY;
            //             } else {
            //                 // 같은 데이터가 다시 수신된 경우
            //                 if(!send_another_wifi_data) {
            //                     send_ble_message("send another wifi data\n");
            //                     send_another_wifi_data = true;
            //                 }
            //                 set_new_ble_data_received(false); // 플래그 리셋
            //             }
            //         }
            //     }
            //     vTaskDelay(pdMS_TO_TICKS(1000));
            //     break;

            // case STATE_DEVICE_DATA_RECEIVE:
            //     ws2812_set_color(0, 255, 0);
            //     // 활성 연결이 모두 끊겼다면 재스캔 상태로 전환
            //     if (ble_device_get_active_connections() == 0) {
            //         ESP_LOGW(GATTS_TAG, "모든 BLE 연결 해제됨 -> 재스캔으로 전환");
            //         current_state = STATE_SCAN_DEVICES;
            //         vTaskDelay(pdMS_TO_TICKS(500));
            //         break;
            //     }
            //     vTaskDelay(pdMS_TO_TICKS(5000));
            //     break;

        //     case STATE_WEBSOCKET_INIT :
        //         // ESP_LOGI(GATTS_TAG, "###### STATE_WEBSOCKET_INIT 진입 ######");
        //         if(!is_wifi_initialized()) {
        //             ESP_ERROR_CHECK(wifi_init());
        //         }
        //         if(!wifi_connected) {
        //             wifi_connected = connect_to_wifi(wifi_id_from_nvs, wifi_pw_from_nvs, wifi_ip_address);
        //             if(wifi_connected) {
        //                 ESP_LOGI(GATTS_TAG, "WiFi IP 주소: %s", wifi_ip_address);
        //                 // char json_data[256];
        //                 // snprintf(json_data, sizeof(json_data),
        //                 // "{\"ip\":\"%s\"}",
        //                 // wifi_ip_address);
        //                 // send_http_post("/receiveIpAddress",json_data);
        //             }
        //         }
                
        //         if(!websocket_server_is_initialized()) {
        //             esp_err_t ret = websocket_server_init();
        //             if (ret == ESP_OK) {
        //                 ESP_LOGI(GATTS_TAG, "WebSocket 초기화 성공");
        //             } else {
        //                 ESP_LOGE(GATTS_TAG, "WebSocket 초기화 실패: %s", esp_err_to_name(ret));
        //             }
        //         }

        //         if(!websocket_server_is_running()) {
        //             esp_err_t ret = websocket_server_start();
        //             if (ret == ESP_OK) {
        //                 ESP_LOGI(GATTS_TAG, "WebSocket 서버 시작");
        //             } else {
        //                 ESP_LOGE(GATTS_TAG, "WebSocket 서버 시작 실패: %s", esp_err_to_name(ret));
        //             }
        //         }
        //         websocket_send_data("hello from ESP32");
        //         vTaskDelay(pdMS_TO_TICKS(50000));
        //         break;

                //   case STATE_MQTT_DATA_SEND:
                // // MQTT 초기화 확인
                // if (!mqtt_is_init()) {
                //     ESP_LOGW(GATTS_TAG, "MQTT 미초기화 - 초기화 상태로 전환");
                //     current_state = STATE_MQTT_INIT;
                //     break;
                // }
                
                // // 데이터 전송
                // esp_err_t send_ret = mqtt_data_send(wifi_ip_address);
                // if (send_ret != ESP_OK) {
                //     ESP_LOGW(GATTS_TAG, "MQTT 데이터 전송 실패: %s", esp_err_to_name(send_ret));
                // }
                
                // // 1초마다 전송
                // vTaskDelay(pdMS_TO_TICKS(10));
                // break;
        }

        // vTaskDelay(pdMS_TO_TICKS(100));
    }
}
