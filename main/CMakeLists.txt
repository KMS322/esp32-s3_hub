# Must run before idf_build_executable() (inside project()); root CMakeLists runs too late.
if(CONFIG_SPIRAM)
    idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=esp_psram_impl_enable" APPEND)
endif()

set(MAIN_SRCS
    "hub_sntp.c" "hub_led.c" "usb.c" "mqtt.c" "device_collector.c" "ble_device.c" "gpio_control.c"
    "ble_client.c" "nvs_manager.c" "main.c" "http_client.c" "ble_gatt.c" "wifi_manager.c"
    "web_socket.c")
if(CONFIG_SPIRAM)
    list(APPEND MAIN_SRCS "hub_sntp.c" "psram_impl_enable_wrap.c")
endif()

idf_component_register(SRCS ${MAIN_SRCS}
                    INCLUDE_DIRS "."
                    REQUIRES "esp_http_client" "nvs_flash" "esp_wifi" "esp_netif" "esp_event" "esp_coex" "bt" "mbedtls" "esp_http_server" "freertos" "esp_common" "esp_timer" "heap" "led_strip" "mqtt" "driver" "cjson")