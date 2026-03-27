#ifndef HUB_LED_H
#define HUB_LED_H

#include <stdint.h>
#include <stdbool.h>

/** 기본 WS2812 밝기 (0~255). 현장에서 식별하기 쉬운 수준으로 조정 가능 */
#define HUB_LED_DEFAULT_BRIGHTNESS 40

/**
 * 정상 동작 모드 (참고: gatt_server_251105/led_color.txt 와 유사한 색·역할).
 * 이 허브는 초기 설정이 USB CDC 기반이라 BLE 광고 단계는 "USB 대기"로 매핑함.
 */
typedef enum {
    HUB_LED_MODE_BOOT_NO_WIFI,    /**< NVS에 Wi-Fi 없음 — 설정 필요 */
    HUB_LED_MODE_USB_WAIT,        /**< USB로 Wi-Fi 문자열 받기 전(또는 CDC 미준비) — 청록 1초 주기 깜빡임 */
    HUB_LED_MODE_USB_LINE_OK,     /**< USB 준비됨, PC에서 설정 문자열 입력 대기 — 청록 고정 */
    HUB_LED_MODE_WIFI_CONNECTING, /**< Wi-Fi 연결 시도 중 — 주황 깜빡임 */
    HUB_LED_MODE_HTTP_CHECKING,   /**< 서버(/check/hub) 등록 확인 중 — 노랑 깜빡임 */
    HUB_LED_MODE_MQTT_CONNECTING, /**< MQTT 브로커 연결 시도 중 — 초록 깜빡임(느리게, 고정과 구분) */
    HUB_LED_MODE_ONLINE,          /**< MQTT 수신 대기 등 정상 유휴 — 초록 고정 */
    HUB_LED_MODE_BLE_SCAN,        /**< BLE 주변기기 스캔·연결 시도 — 파랑 깜빡임 */
    HUB_LED_MODE_BLE_CONNECTED,   /**< BLE 다중 연결 직후(잠시) — 파랑 고정 */
} hub_led_mode_t;

/**
 * 오류 패턴 (참고 파일과 동일한 원리: 빨강 N회 → 구분색 M회 반복).
 * 정상 모드로 복귀하면(hub_led_set_mode) 오류 표시는 해제됨.
 */
typedef enum {
    HUB_LED_ERR_NONE = 0,
    HUB_LED_ERR_HTTP,             /**< 빨강(3) → 노랑(3) */
    HUB_LED_ERR_MQTT_INIT,        /**< 빨강(3) → 보라(3) */
    HUB_LED_ERR_MQTT_CONNECT,     /**< 빨강(3) → 보라(2) — 연결 시간 초과 등 */
    HUB_LED_ERR_MQTT_PUBLISH,     /**< 빨강(3) → 보라(1) */
    HUB_LED_ERR_MQTT_TRANSPORT,   /**< 빨강(3) → 보라(3) — 브로커 TCP/전송 오류 */
    HUB_LED_ERR_WIFI,             /**< 빨강(3) → 주황(3) */
    HUB_LED_ERR_BLE_SCAN,         /**< 빨강(3) → 파랑(3) */
    HUB_LED_ERR_BLE_CONNECT,      /**< 빨강(3) → 파랑(2) */
    HUB_LED_ERR_BLE_SEND,         /**< 빨강(3) → 파랑(1) */
    HUB_LED_ERR_NVS_SAVE,         /**< 빨강(3) → 청록(3) */
    HUB_LED_ERR_USB_CDC,          /**< 빨강(3) → 마젠타(3) — USB CDC 초기화 실패(참고 파일에 없음, USB 전용) */
    HUB_LED_ERR_MEMORY,           /**< 빨강(5) → 1초 대기 반복 */
} hub_led_err_t;

/** hub_led 전용 태스크 시작, 밝기 설정. gpio_control_init() 이후 호출 */
void hub_led_init(uint8_t brightness);

/** 현재 정상 모드 설정(오류 표시 해제) */
void hub_led_set_mode(hub_led_mode_t mode);

/** 오류 패턴 표시(정상 모드보다 우선) */
void hub_led_set_error(hub_led_err_t err);

/** 오류 표시만 해제하고 현재 모드 유지 */
void hub_led_clear_error(void);

/** 이전 모드와 같으면 WS2812 갱신 생략(부하 감소) */
void hub_led_set_mode_if_changed(hub_led_mode_t mode);

#endif /* HUB_LED_H */
