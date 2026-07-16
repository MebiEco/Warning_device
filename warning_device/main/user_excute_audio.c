#include "user_excute_audio.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "PLAYER";

uint8_t *psram_audio_buffer = NULL;
size_t psram_audio_len = 0;
size_t psram_audio_pos = 0;

int play_state = 0; // 0=Idle, 1=Playing, 2=Waiting
uint32_t wait_ticks = 0;
AlarmCmd_t current_cmd;
int file_to_play = 0;

bool is_playing = false;
QueueHandle_t alarm_queue = NULL;
QueueHandle_t siren_queue = NULL;

void Stop_Alarm(void)
{
  if (alarm_queue != NULL)
  {
    xQueueReset(alarm_queue);
  }
  is_playing = false;
  play_state = 0;
  psram_audio_len = 0;
  psram_audio_pos = 0;
  ESP_LOGW("ALARM", "Alarm stopped immediately. Queue cleared.");
}

void sd_read_task(void *pvParameters)
{
  FILE *f_music = NULL;

  psram_audio_buffer = malloc(MAX_AUDIO_BUFFER_SIZE);
  if (!psram_audio_buffer)
  {
    ESP_LOGE(TAG, "Failed to allocate 2MB static buffer in PSRAM!");
    vTaskDelete(NULL);
  }

  if (alarm_queue == NULL)
  {
    alarm_queue = xQueueCreate(10, sizeof(AlarmCmd_t));
  }

  while (1)
  {
    if (play_state == 0)
    {
      if (xQueueReceive(alarm_queue, &current_cmd, 0) == pdTRUE)
      {
        is_playing = false; // Pause playback
        play_state = 1;     // Immediately enter playing state
        wait_ticks = 0;

        psram_audio_len = 0;
        psram_audio_pos = 0;

        char path[64];
        snprintf(path, sizeof(path), "/sdcard/%d.wav", current_cmd.file_id);
        f_music = fopen(path, "rb");
        if (f_music)
        {
          fseek(f_music, 0, SEEK_END);
          long file_size = ftell(f_music);
          fseek(f_music, 44, SEEK_SET);

          long audio_size = file_size - 44;
          if (audio_size > 0)
          {
            if (audio_size > MAX_AUDIO_BUFFER_SIZE)
            {
              ESP_LOGW(TAG, "File too large (%ld bytes). Truncating to 2MB.", audio_size);
              audio_size = MAX_AUDIO_BUFFER_SIZE;
            }

            ESP_LOGI(TAG, "Reading %ld bytes into static PSRAM buffer...", audio_size);
            size_t read_bytes = fread(psram_audio_buffer, 1, audio_size, f_music);
            if (read_bytes > 0)
            {
              psram_audio_len = read_bytes;
              psram_audio_pos = 0;
              is_playing = true;
              ESP_LOGI(TAG, "Successfully loaded and playing: %s", path);
            }
            else
            {
              ESP_LOGE(TAG, "Failed to read data from file");
              play_state = 0;
            }
          }
          fclose(f_music);
          f_music = NULL;
        }
        else
        {
          ESP_LOGE(TAG, "Cannot open file: %s", path);
          play_state = 0;
        }
      }
    }

    if (play_state == 1)
    {
      if (!is_playing && psram_audio_buffer != NULL)
      {
        if (current_cmd.repeat_count > 1)
        {
          current_cmd.repeat_count--;
          play_state = 2;                             
          wait_ticks = current_cmd.duration_sec * 10; 
          ESP_LOGI(TAG, "Audio finished. Waiting %d seconds for next repeat...", current_cmd.duration_sec);
        }
        else
        {
          play_state = 0; 
          ESP_LOGI(TAG, "All alarm repeats finished. Idle.");
          psram_audio_len = 0;
          psram_audio_pos = 0;
        }
      }
    }
    else if (play_state == 2)
    {
      if (wait_ticks > 0)
      {
        wait_ticks--;
      }
      else
      {
        ESP_LOGI(TAG, "Resuming alarm repeat...");
        psram_audio_pos = 0; 
        is_playing = true;   
        play_state = 1;      
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void Bell_Task(void *pvParameters)
{
  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << BELL_GPIO_PIN),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&io_conf);
  gpio_set_level(BELL_GPIO_PIN, 0);

  if (siren_queue == NULL)
  {
    siren_queue = xQueueCreate(5, sizeof(SirenCmd_t));
  }

  SirenCmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  bool bell_running = false; 
  int remaining_repeats = 0;

  while (1)
  {
    if (xQueueReceive(siren_queue, &cmd, 0) == pdTRUE)
    {
      if (cmd.value == 0)
      {
        gpio_set_level(BELL_GPIO_PIN, 0);
        bell_running = false;
        remaining_repeats = 0;
        ESP_LOGI("BELL", "Bell stopped by command.");
      }
      else
      {
        remaining_repeats = cmd.repeat; 
        bell_running = (cmd.repeat == 0);
        ESP_LOGI("BELL", "Bell start: duration=%ds rest=%ds repeat=%d",
                 cmd.duration_sec, cmd.rest_sec, cmd.repeat);
      }
    }

    if (remaining_repeats > 0 || bell_running)
    {
      gpio_set_level(BELL_GPIO_PIN, 1);
      ESP_LOGD("BELL", "Bell ON");

      TickType_t on_start = xTaskGetTickCount();
      bool stopped = false;
      while ((xTaskGetTickCount() - on_start) < pdMS_TO_TICKS(cmd.duration_sec * 1000))
      {
        SirenCmd_t stop_cmd;
        if (xQueuePeek(siren_queue, &stop_cmd, 0) == pdTRUE && stop_cmd.value == 0)
        {
          xQueueReceive(siren_queue, &stop_cmd, 0);
          gpio_set_level(BELL_GPIO_PIN, 0);
          bell_running = false;
          remaining_repeats = 0;
          ESP_LOGI("BELL", "Bell stopped mid-ring.");
          stopped = true;
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      if (stopped)
      {
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
      }

      gpio_set_level(BELL_GPIO_PIN, 0);
      ESP_LOGD("BELL", "Bell OFF");

      if (remaining_repeats > 0)
      {
        remaining_repeats--;
      }

      if (remaining_repeats > 0 || bell_running)
      {
        TickType_t rest_start = xTaskGetTickCount();
        while ((xTaskGetTickCount() - rest_start) < pdMS_TO_TICKS(cmd.rest_sec * 1000))
        {
          SirenCmd_t stop_cmd;
          if (xQueuePeek(siren_queue, &stop_cmd, 0) == pdTRUE && stop_cmd.value == 0)
          {
            xQueueReceive(siren_queue, &stop_cmd, 0);
            bell_running = false;
            remaining_repeats = 0;
            ESP_LOGI("BELL", "Bell stopped during rest.");
            stopped = true;
            break;
          }
          vTaskDelay(pdMS_TO_TICKS(20));
        }
      }
      else
      {
        ESP_LOGI("BELL", "Bell sequence complete.");
      }
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}
