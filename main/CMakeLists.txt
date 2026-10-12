set(srcs
    "main.c"
    # Config (AI API keys/URLs, overridable via config.json)
    "config/ai_config.c"
    # Input (button polling)
    "input/button.c"
    # Display (UC8179 EPD + SPI I/O + Stucki dither)
    "display/epd.c"
    "display/io.c"
    "display/dither.c"
    # Network (WiFi manager + AI API client)
    "network/wifi_manager.c"
    "network/ai_client.c"
    # Image decoder (HTTPS download + PNG decode + crop/scale → RGB565)
    "image/lodepng.c"
    "image/image_decoder.c"
    "image/image_processing.c"
    "image/color_space.c"
    # Web (provisioning HTTP server + admin server + split API handlers)
    "web/web_server.c"
    "web/admin_server.c"
    "web/api_core.c"
    "web/api_content.c"
    "web/api_files.c"
    # Power (light sleep + RTC-timer/GPIO wakeup)
    "power/sleep_manager.c"
    # Text rendering (FreeType Chinese text overlay)
    "text/text_renderer.c"
    # Storage (SD card + config store + image/quote library + daily record + log)
    "storage/sd_card.c"
    "storage/config_store.c"
    "storage/image_library.c"
    "storage/quote_library.c"
    "storage/daily.c"
    "storage/log_to_sd.c"
    # OTA (firmware upgrade via esp_ota_ops)
    "ota/ota_manager.c"
    # Content (image+quote selection, AI pipeline, daily task)
    "content/daily_content.c"
    )

idf_component_register(SRCS ${srcs}
                    INCLUDE_DIRS "."
                                 "display"
                                 "network"
                                 "web"
                                 "image"
                                 "config"
                                 "events"
                                 "storage"
                                 "text"
                                 "power"
                                 "input"
                                 "ota"
                                 "content"
                    PRIV_REQUIRES
                        esp_driver_spi
                        esp_driver_gpio
                        nvs_flash
                        esp_wifi
                        esp_netif
                        esp_event
                        esp_http_server
                        esp_http_client
                        mbedtls
                        freetype
                        fatfs
                        sdmmc
                        esp_driver_sdspi
                        app_update
                        esp_app_format
)