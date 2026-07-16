idf_component_register(SRCS "DAC_I2S.c" "user_ota.c" "main.c"
        "user_http_server.c"
        "user_system.c"
        "user_time.c"
        "user_sdcard.c"
        "bt_app_core.c"
        "user_azure.c"
        "user_wifi.c"
        "user_bluetooth.c"
        "user_excute_audio.c"
        "DAC_I2S.c"
                    INCLUDE_DIRS "."
                    
                    REQUIRES sdmmc
                                fatfs
                                nvs_flash
                                spiffs
                                driver
                                bt
                                esp_http_server
                                esp_wifi
                                coreMQTT
                                azure-sdk-for-c 
                                azure-iot-middleware-freertos 
                                sample-azure-iot 
                                json
                                esp_https_ota
                                app_update
                    )

# spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)