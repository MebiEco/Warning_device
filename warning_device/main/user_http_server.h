#pragma once
#include "stdint.h"
#include "stdbool.h"
#include "esp_gap_bt_api.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "user_excute_audio.h"
#include "user_bluetooth.h"

void User_Http_Server_Task(void);

/* Bell/Siren task - chạy độc lập, nhận lệnh từ siren_queue */
void Bell_Task(void *pvParameters);