#include "DAC_I2S.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "user_excute_audio.h"
#include <string.h>

static const char *TAG = "DAC_I2S";
static i2s_chan_handle_t tx_chan;

void DAC_I2S_Init(void)
{
    ESP_LOGI(TAG, "Initializing I2S TX channel");
    
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    /*
     * Cấu hình chân I2S theo sơ đồ nguyên lý:
     * - BCLK (SCK / BCK): GPIO 19
     * - WS (LRCK / LCK): GPIO 22
     * - DOUT (DIN): GPIO 21
     */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100), // Standard 44.1kHz sampling rate
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = 19,
            .ws   = 22,
            .dout = 21,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
    
    ESP_LOGI(TAG, "I2S initialized successfully on BCLK:19, WS:22, DOUT:21");
}

void DAC_I2S_Deinit(void)
{
    if (tx_chan != NULL)
    {
        ESP_LOGI(TAG, "Deinitializing I2S TX channel");
        esp_err_t err = i2s_channel_disable(tx_chan);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to disable I2S channel: %s", esp_err_to_name(err));
        }
        err = i2s_del_channel(tx_chan);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete I2S channel: %s", esp_err_to_name(err));
        }
        tx_chan = NULL;
        ESP_LOGI(TAG, "I2S deinitialized successfully");
    }
}

extern bool sys_is_dac_i2s_mode;
extern TaskHandle_t DAC_I2S_Task_Handle;

void DAC_I2S_Task(void *pvParameters)
{
    DAC_I2S_Init();
    
    uint8_t buffer[1024];
    size_t bytes_written = 0;

    ESP_LOGI(TAG, "I2S Player task started. Waiting for audio data...");

    while (sys_is_dac_i2s_mode) {
        if (!is_playing || psram_audio_buffer == NULL) {
            // Fill with silence if nothing is playing to maintain clock and avoid noise
            memset(buffer, 0, sizeof(buffer));
            i2s_channel_write(tx_chan, buffer, sizeof(buffer), &bytes_written, portMAX_DELAY);
        } else {
            // Data is available, write to I2S
            size_t remain = psram_audio_len - psram_audio_pos;
            if (remain >= sizeof(buffer)) {
                i2s_channel_write(tx_chan, psram_audio_buffer + psram_audio_pos, sizeof(buffer), &bytes_written, portMAX_DELAY);
                psram_audio_pos += bytes_written;
            } else if (remain > 0) {
                memset(buffer, 0, sizeof(buffer));
                memcpy(buffer, psram_audio_buffer + psram_audio_pos, remain);
                i2s_channel_write(tx_chan, buffer, sizeof(buffer), &bytes_written, portMAX_DELAY);
                psram_audio_pos += remain;
                
                // End of playback
                is_playing = false;
                ESP_LOGW(TAG, "Finished playing via I2S DAC. Waiting for next command.");
            } else {
                is_playing = false;
            }
        }
    }

    ESP_LOGI(TAG, "I2S Player task exiting...");
    DAC_I2S_Deinit();
    DAC_I2S_Task_Handle = NULL;
    vTaskDelete(NULL);
}
