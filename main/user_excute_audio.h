#ifndef USER_EXCUTE_AUDIO_H
#define USER_EXCUTE_AUDIO_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdint.h>
#include <stdbool.h>

#define MAX_AUDIO_BUFFER_SIZE (2 * 1024 * 1024) // 2MB fixed buffer
#define BELL_GPIO_PIN 32

extern uint8_t *psram_audio_buffer;
extern size_t psram_audio_len;
extern size_t psram_audio_pos;

extern int play_state; // 0=Idle, 1=Playing, 2=Waiting
extern uint32_t wait_ticks;
extern int file_to_play;
extern bool is_playing;

typedef struct {
  int file_id;
  char filename[64]; /* nếu không rỗng → phát /sdcard/<filename> */
  int duration_sec;
  int repeat_count;
} AlarmCmd_t;

typedef struct {
  int value;
  int duration_sec;
  int rest_sec;
  int repeat;
} SirenCmd_t;

extern QueueHandle_t alarm_queue;
extern QueueHandle_t siren_queue;
extern AlarmCmd_t current_cmd;

void sd_read_task(void *pvParameters);
void Bell_Task(void *pvParameters);
void Stop_Alarm(void);

#endif // USER_EXCUTE_AUDIO_H
