#include "user_time.h"
#include "driver/gptimer.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "time.h"
#include "esp_netif_sntp.h"
#include "esp_netif.h"
#include "user_system.h"

#include "esp_event.h"
#include "esp_wifi.h"

const char *TIMER_TAG = "TIMER: ";

bool IRAM_ATTR Timer_On_Alarm_Cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t *eData, void *userCtx)
{
    Sys_Info.isTimeSyncCb = true;
    return true;
}

void User_Timer_Init(void)
{
    gptimer_handle_t gptimer = NULL;
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));

    gptimer_alarm_config_t alarm_config = {
        .reload_count = 0,
        .alarm_count = 1000000,
        .flags.auto_reload_on_alarm = true,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config));

    gptimer_event_callbacks_t cbs = {
        .on_alarm = Timer_On_Alarm_Cb,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, NULL));
    ESP_ERROR_CHECK(gptimer_enable(gptimer));
    ESP_ERROR_CHECK(gptimer_start(gptimer));
}

static void time_sync_cb(struct timeval *tv)
{
    ESP_LOGI(TIMER_TAG, "Time is synchronized successfully");
    Sys_Info.isTimeSync = true;
}

void User_Get_time(void)
{
    while (!Sys_Info.isWifiConnected) {
        ESP_LOGW(TIMER_TAG, "Wait for wifi connected");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    /* Nhiều server (cần CONFIG_LWIP_SNTP_MAX_SERVERS>=3); tắt delay khởi động gây chậm boot */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        3,
        ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com", "time.cloudflare.com"));
    cfg.sync_cb = time_sync_cb;
    cfg.wait_for_sync = true;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TIMER_TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return;
    }

    /* Chờ tối đa 15s — tránh treo boot vô hạn nếu DNS/NTP nghẽn */
    const int max_wait_ms = 15000;
    int waited = 0;
    while (!Sys_Info.isTimeSync && waited < max_wait_ms) {
        err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000));
        waited += 2000;
        if (err == ESP_OK || Sys_Info.isTimeSync) {
            break;
        }
        ESP_LOGW(TIMER_TAG, "Waiting NTP... (%d/%d ms)", waited, max_wait_ms);
    }

    if (!Sys_Info.isTimeSync) {
        ESP_LOGE(TIMER_TAG, "NTP timeout — continuing without sync (Azure SAS may fail)");
        /* Không force isTimeSync: Azure/BT có thể retry sau khi sync nền thành công qua callback */
    }

    time_t now;
    char timeBuf[64];
    struct tm timeInfo;

    time(&now);
    setenv("TZ", "UTC-7", 1);
    tzset();
    localtime_r(&now, &timeInfo);
    strftime(timeBuf, sizeof(timeBuf), "%c", &timeInfo);
    ESP_LOGI("TIME: ", "The current date/time is: %s", timeBuf);
}

void User_Time_Task(void)
{
    User_Timer_Init();
    User_Get_time();

    vTaskDelay(pdMS_TO_TICKS(10000));

    while (1) {
        if (Sys_Info.isTimeSyncCb) {
            Sys_Info.isTimeSyncCb = false;
            time(&Sys_Info.epochtime);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
