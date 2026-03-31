/*
 * After esp_psram_impl_enable() reprograms shared MSPI for octal PSRAM, cached XIP at ~0x4200xxxx
 * can disagree with flash until caches are invalidated. Linked with -Wl,--wrap=esp_psram_impl_enable.
 * This file is only compiled when CONFIG_SPIRAM (see main/CMakeLists.txt).
 */
#include "esp_attr.h"
#include "esp_err.h"
#include "esp32s3/rom/cache.h"

/* esp_psram_impl.h is not a public IDF header; keep in sync with IDF enum. */
typedef enum {
    PSRAM_VADDR_MODE_NORMAL = 0,
    PSRAM_VADDR_MODE_LOWHIGH,
    PSRAM_VADDR_MODE_EVENODD,
} psram_vaddr_mode_t;

esp_err_t __real_esp_psram_impl_enable(psram_vaddr_mode_t vaddrmode);

esp_err_t IRAM_ATTR __wrap_esp_psram_impl_enable(psram_vaddr_mode_t vaddrmode)
{
    esp_err_t r = __real_esp_psram_impl_enable(vaddrmode);
    if (r == ESP_OK) {
        Cache_Invalidate_ICache_All();
        Cache_Invalidate_DCache_All();
        __asm__ __volatile__("memw" ::: "memory");
    }
    return r;
}
