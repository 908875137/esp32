set(srcs
    "main.c"
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
    # Web (provisioning HTTP server)
    "web/web_server.c"
    # Input (button polling task)
    "input/button.c"
    # Text rendering (FreeType Chinese text overlay)
    "text/text_renderer.c"
    )

idf_component_register(SRCS ${srcs}
                    INCLUDE_DIRS "."
                                 "display"
                                 "network"
                                 "web"
                                 "image"
                                 "input"
                                 "config"
                                 "events"
                                 "storage"
                                 "text"
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
                        spiffs
                        freetype
)

# SPIFFS image: embed the fonts/ directory into the "fonts" partition (10MB)
# FreeType opens the font via /spiffs/NotoSansCJKsc-Subset.otf
spiffs_create_partition_image(fonts ../fonts FLASH_IN_PROJECT)