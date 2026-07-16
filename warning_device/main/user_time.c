#include "user_time.h"
#include "driver/gptimer.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "time.h"
#include "esp_sntp.h"
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
        .clk_src = GPTIMER_CLK_SRC_DEFAULT, // Select the default clock source
        .direction = GPTIMER_COUNT_UP,      // Counting direction is up
        .resolution_hz = 1 * 1000 * 1000,   // Resolution is 1 MHz, i.e., 1 tick equals 1 microsecond
    };
    // Create a timer instance
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));

    gptimer_alarm_config_t alarm_config = {
        .reload_count = 0,      // When the alarm event occurs, the timer will automatically reload to 0
        .alarm_count = 1000000, // Set the actual alarm period, since the resolution is 1us, 1000000 represents 1s
        .flags.auto_reload_on_alarm = true, // Enable auto-reload function
    };
    // Set the timer's alarm action
    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config));

    gptimer_event_callbacks_t cbs = {
        .on_alarm = Timer_On_Alarm_Cb, // Call the user callback function when the alarm event occurs
    };
    // Register timer event callback functions, allowing user context to be carried
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, NULL));
    // Enable the timer
    ESP_ERROR_CHECK(gptimer_enable(gptimer));
    // Start the timer
    ESP_ERROR_CHECK(gptimer_start(gptimer));
}

static void time_sync_cb(struct timeval *tv)
{
    ESP_LOGI(TIMER_TAG, "Time is synchronized successfully\n\n\n");
    Sys_Info.isTimeSync = true;
}

void User_Get_time(void)
{
    while(!Sys_Info.isWifiConnected)
    {   
        ESP_LOGW(TIMER_TAG, "Wait for wifi connected");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    time_t now;

    char timeBuf[64];

    struct  tm timeInfo;
    
    esp_sntp_setservername(0, "time.google.com");
    esp_sntp_setservername(1, "time1.google.com");
    esp_sntp_setservername(2, "time2.google.com");


    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_set_sync_interval(3600000);

    esp_sntp_set_time_sync_notification_cb(time_sync_cb);

    esp_sntp_init();

    while(!Sys_Info.isTimeSync)
    {
        ESP_LOGW(TIMER_TAG, "Wait time is synchronized ... ");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

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

    while (1)
    {
        if(Sys_Info.isTimeSyncCb)
        {
            static uint8_t state = 0x01;

            state ^= 0x01;

            Sys_Info.isTimeSyncCb = false;
            time(&Sys_Info.epochtime);

        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
}


