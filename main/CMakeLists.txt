idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES
                        esp_wifi
                        nvs_flash
                        esp_event
                        esp_http_client
                        driver
                        usb
                        spiffs
                        # cjson
                        # espressif__cjson
                        # espressif__esp-sr
                        cjson
                        esp-sr
                        esp_tinyusb
                        mbedtls
                    )