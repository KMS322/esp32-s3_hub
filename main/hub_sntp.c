#include "hub_sntp.h"

#if !HUB_SNTP_ENABLED

void hub_sntp_after_wifi_got_ip(char *wifi_ip_buf, size_t wifi_ip_sz, const char *log_tag)
{
    (void)wifi_ip_buf;
    (void)wifi_ip_sz;
    (void)log_tag;
}

#else

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"

#include "wifi_manager.h"

#ifndef HUB_SNTP_SERVER
#define HUB_SNTP_SERVER "pool.ntp.org"
#endif

#ifndef HUB_SNTP_SYNC_TIMEOUT_MS
#define HUB_SNTP_SYNC_TIMEOUT_MS 5000
#endif

static const char *s_fallback_tag = "HUB_SNTP";
static bool s_sntp_inited;
/** IP 없음으로 스킵한 뒤, 한 번이라도 대기 루프(성공/타임아웃)를 돌았으면 true — HTTP 반복 진입 시 5초 대기 방지 */
static bool s_sync_wait_finished;

static bool ip_str_valid(const char *ip)
{
    return ip != NULL && ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0;
}

static void refresh_sta_ip(char *buf, size_t buflen)
{
    if (buf == NULL || buflen == 0) {
        return;
    }
    esp_netif_t *netif = wifi_get_sta_netif();
    if (netif == NULL) {
        return;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return;
    }
    if (ip_info.ip.addr == 0) {
        return;
    }
    inet_ntoa_r(ip_info.ip, buf, buflen);
}

static void log_local_time(const char *tag)
{
    struct timeval tv;
    struct tm ti;

    if (gettimeofday(&tv, NULL) != 0) {
        ESP_LOGW(tag, "gettimeofday 실패");
        return;
    }
    if (localtime_r(&tv.tv_sec, &ti) == NULL) {
        ESP_LOGW(tag, "localtime_r 실패");
        return;
    }
    char line[48];
    if (strftime(line, sizeof(line), "%Y-%m-%d %H:%M:%S", &ti) == 0) {
        ESP_LOGW(tag, "strftime 실패");
        return;
    }
    ESP_LOGI(tag, "현재 시각(KST): %s.%03ld", line, (long)(tv.tv_usec / 1000));
}

void hub_sntp_after_wifi_got_ip(char *wifi_ip_buf, size_t wifi_ip_sz, const char *log_tag)
{
    const char *tag = (log_tag != NULL) ? log_tag : s_fallback_tag;

    if (wifi_ip_buf != NULL && wifi_ip_sz > 0) {
        if (!ip_str_valid(wifi_ip_buf)) {
            refresh_sta_ip(wifi_ip_buf, wifi_ip_sz);
        }
    }

    if (!ip_str_valid(wifi_ip_buf)) {
        ESP_LOGW(tag, "SNTP 건너뜀: 유효한 STA IP 없음 (DHCP 후 STATE_MAC_ADDRESS_CHECK에서 재시도)");
        return;
    }

    if (!s_sntp_inited) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(HUB_SNTP_SERVER);
        esp_err_t err = esp_netif_sntp_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(tag, "esp_netif_sntp_init 실패: %s", esp_err_to_name(err));
            return;
        }
        s_sntp_inited = true;
    }

    if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        return;
    }
    if (s_sync_wait_finished) {
        return;
    }
    s_sync_wait_finished = true;

    for (int elapsed = 0; elapsed < HUB_SNTP_SYNC_TIMEOUT_MS; elapsed += 100) {
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            ESP_LOGI(tag, "SNTP 동기 완료");
            log_local_time(tag);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
        ESP_LOGW(tag, "SNTP 동기 시간 초과 (%d ms) — 시각이 아직 부정확할 수 있음", HUB_SNTP_SYNC_TIMEOUT_MS);
    }

    log_local_time(tag);
}

#endif /* HUB_SNTP_ENABLED */
