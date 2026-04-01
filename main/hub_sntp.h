/**
 * Wi‑Fi STA IP 확보 후 SNTP 동기화 및 시각 로그.
 *
 * 롤백(이전 동작 = SNTP 없음): 아래 매크로를 0으로 바꾼 뒤 재빌드하면
 * hub_sntp_after_wifi_got_ip()는 즉시 no-op 이 되며 main.c 호출부는 그대로 둬도 됩니다.
 */
#ifndef HUB_SNTP_H
#define HUB_SNTP_H

#include <stddef.h>

#ifndef HUB_SNTP_ENABLED
#define HUB_SNTP_ENABLED 1
#endif

/**
 * STA에 유효한 IP가 있을 때만 SNTP를 시작하고, 짧은 상한 안에 동기를 기다린 뒤
 * 현재 시각을 로그로 출력합니다.
 *
 * @param wifi_ip_buf  connect_to_wifi 등이 채운 버퍼; 0.0.0.0 이면 netif에서 다시 읽습니다.
 * @param wifi_ip_sz   wifi_ip_buf 크기
 * @param log_tag      ESP_LOG 태그 (NULL 이면 "HUB_SNTP")
 */
void hub_sntp_after_wifi_got_ip(char *wifi_ip_buf, size_t wifi_ip_sz, const char *log_tag);

#endif /* HUB_SNTP_H */
