#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"

#include "user_system.h"
#include "user_http_server.h"
#include "user_sdcard.h"
#include "user_azure.h"
#include "user_time.h"
#include "user_ota.h"
#include "DAC_I2S.h"

#include "user_wifi.h"
#include "user_bluetooth.h"
#include "user_excute_audio.h"

static const char *TAG = "main";

TaskHandle_t Http_Server_Task_Handle = NULL;
TaskHandle_t Azure_Task_Handle = NULL;
TaskHandle_t SDCard_Task_Handle = NULL;
TaskHandle_t Timer_Task_Handle = NULL;
TaskHandle_t DAC_I2S_Task_Handle = NULL;

static void log_heap(const char *stage)
{
  ESP_LOGI(TAG, "[HEAP @ %s] free=%" PRIu32 " internal=%" PRIu32 " largest=%" PRIu32,
           stage,
           (uint32_t)esp_get_free_heap_size(),
           (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void my_spiffc_init(void)
{
  ESP_LOGI(TAG, "Initializing SPIFFS");

  esp_vfs_spiffs_conf_t conf = {.base_path = "/spiffs",
                                .partition_label = "storage",
                                .max_files = 5,
                                .format_if_mount_failed = true};
  esp_err_t ret = esp_vfs_spiffs_register(&conf);

  if (ret != ESP_OK)
  {
    if (ret == ESP_FAIL)
    {
      ESP_LOGE(TAG, "Failed to mount or format filesystem");
    }
    else if (ret == ESP_ERR_NOT_FOUND)
    {
      ESP_LOGE(TAG, "Failed to find SPIFFS partition");
    }
    else
    {
      ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
    }
    return;
  }

  size_t total = 0, used = 0;
  ret = esp_spiffs_info("storage", &total, &used);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
  }
  else
  {
    ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
  }
}

void list_files(const char *base_path)
{
  printf("\n\n");
  DIR *dir = opendir(base_path);
  if (dir == NULL)
  {
    ESP_LOGE("SPIFFS", "Failed to open directory: %s", base_path);
    return;
  }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL)
  {
    printf("%s\n", entry->d_name);
    if (entry->d_type == DT_DIR)
    {
      if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
      {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", base_path, entry->d_name);
        list_files(path);
      }
    }
  }
  closedir(dir);
}

void Timer_Task(void *pvParameters)
{
    User_Time_Task();
}

void SDCard_Task(void *pvParameters)
{
    User_SDCard_Task();
}

void Azure_Task(void *pvParameters)
{
    User_Azure_Task();
}

void Http_Server_Task(void *pvParameters)
{
    User_Http_Server_Task();
}

/* ─────────────────────────────────────────────────────────
 *  SWITCH & LED PIN DEFINITIONS
 *  Switch: Active LOW (kéo xuống GND khi gạt ON)
 *  LED:    Active HIGH
 * ───────────────────────────────────────────────────────── */

// Switch pins
// LƯU Ý: GPIO12 là chân strapping — phải LOW lúc reset nếu flash 3.3V.
// Nếu SW2 OFF (pull-up → HIGH) board có thể không boot được.
#define SW1_CONFIG_PIN   13   // Gạt ON → Config/AP mode
#define SW2_DAC_I2S_PIN  12   // Gạt ON → DAC I2S mode
#define SW3_BT_PIN       14   // Gạt ON → Bluetooth mode
#define SW4_RESERVED_PIN 27   // Dự phòng

// LED pins
#define LED1_CONFIG_PIN  33   // Đèn báo chế độ Config/AP
#define LED2_WIFI_PIN    25   // Đèn báo kết nối WiFi/Azure
#define LED3_AUDIO_PIN   26   // Đèn báo chế độ âm thanh

/* ─────────────────────────────────────────────────────────
 *  HÀM: switches_init
 *  MÔ TẢ: Khởi tạo 4 chân switch là input, pull-up nội
 * ───────────────────────────────────────────────────────── */
static void switches_init(void)
{
    gpio_config_t io_conf = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << SW1_CONFIG_PIN)  |
                        (1ULL << SW2_DAC_I2S_PIN) |
                        (1ULL << SW3_BT_PIN)      |
                        (1ULL << SW4_RESERVED_PIN),
        .pull_up_en   = GPIO_PULLUP_ENABLE,   // Pull-up để khi hở mạch = HIGH
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "Switches initialized on GPIO %d, %d, %d, %d",
             SW1_CONFIG_PIN, SW2_DAC_I2S_PIN, SW3_BT_PIN, SW4_RESERVED_PIN);
}

/* ─────────────────────────────────────────────────────────
 *  HÀM: switch_is_on(pin)
 *  MÔ TẢ: Trả về true khi switch đang ở vị trí ON
 *         (gạt ON → kéo GND → đọc mức LOW)
 * ───────────────────────────────────────────────────────── */
static inline bool switch_is_on(int pin)
{
    return gpio_get_level(pin) == 0;
}

extern bool load_wifi_from_nvs(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

bool sys_is_in_config_mode = false;
bool sys_is_dac_i2s_mode    = false;
bool sys_is_bt_mode         = false;
/* false cho đến hết boot — tránh gạt SW2/SW3 giữa lúc init BT gây crash */
static volatile bool sys_boot_complete = false;

/* ─────────────────────────────────────────────────────────
 *  HÀM: leds_init
 *  MÔ TẢ: Khởi tạo 3 chân LED là output
 * ───────────────────────────────────────────────────────── */
static void leds_init(void)
{
    gpio_config_t io_conf = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED1_CONFIG_PIN) |
                        (1ULL << LED2_WIFI_PIN)   |
                        (1ULL << LED3_AUDIO_PIN),
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED1_CONFIG_PIN, 0);
    gpio_set_level(LED2_WIFI_PIN,   0);
    gpio_set_level(LED3_AUDIO_PIN,  0);
    ESP_LOGI(TAG, "LEDs initialized on GPIO %d, %d, %d",
             LED1_CONFIG_PIN, LED2_WIFI_PIN, LED3_AUDIO_PIN);
}

/* ─────────────────────────────────────────────────────────
 *  LED1 Task: Báo trạng thái chế độ Config/AP
 *    - Nháy 1s   : Đang ở Config Mode, chưa có thiết bị kết nối AP
 *    - Nháy 100ms: Đang ở Config Mode, có thiết bị đang kết nối vào AP
 *    - Tắt       : Không ở Config Mode
 * ───────────────────────────────────────────────────────── */
void Led1_Config_Task(void *pvParameters)
{
    int state = 1;
    while (1)
    {
        if (sys_is_in_config_mode)
        {
            wifi_sta_list_t sta_list;
            bool has_client = (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK && sta_list.num > 0);

            state = !state;
            gpio_set_level(LED1_CONFIG_PIN, state);
            // Có thiết bị kết nối → nháy nhanh 100ms; không có → nháy 1s
            vTaskDelay(pdMS_TO_TICKS(has_client ? 100 : 500));
        }
        else
        {
            gpio_set_level(LED1_CONFIG_PIN, 1);
            state = 1;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

/* ─────────────────────────────────────────────────────────
 *  LED2 Task: Báo trạng thái kết nối WiFi / Azure
 *    - Nháy 1s   : Chỉ có WiFi (chưa có Azure)
 *    - Nháy 100ms: Có cả WiFi + Azure
 *    - Tắt       : Không có WiFi
 * ───────────────────────────────────────────────────────── */
void Led2_Wifi_Task(void *pvParameters)
{
    int state = 1;
    while (1)
    {
        if (Sys_Info.isWifiConnected && IoTHubHandle.isAzureInitialized)
        {
            // WiFi + Azure: nháy 100ms
            state = !state;
            gpio_set_level(LED2_WIFI_PIN, state);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        else if (Sys_Info.isWifiConnected)
        {
            // Chỉ WiFi: nháy 1s
            state = !state;
            gpio_set_level(LED2_WIFI_PIN, state);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        else
        {
            // Không có WiFi: tắt
            gpio_set_level(LED2_WIFI_PIN, 1);
            state = 1;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

/* ─────────────────────────────────────────────────────────
 *  LED3 Task: Báo chế độ âm thanh đang chạy
 *    - Nháy 1s   : Đang chạy DAC I2S mode
 *    - Nháy 100ms: Đang chạy Bluetooth mode
 *    - Tắt       : Không ở cả hai chế độ
 * ───────────────────────────────────────────────────────── */
void Led3_Audio_Task(void *pvParameters)
{
    int state = 1;
    while (1)
    {
        if (sys_is_dac_i2s_mode)
        {
            // DAC I2S: nháy 1s
            state = !state;
            gpio_set_level(LED3_AUDIO_PIN, state);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        else if (sys_is_bt_mode)
        {
            // Bluetooth: nháy 100ms
            state = !state;
            gpio_set_level(LED3_AUDIO_PIN, state);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        else
        {
            gpio_set_level(LED3_AUDIO_PIN, 1);
            state = 1;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

/* ─────────────────────────────────────────────────────────
 *  Switch_Monitor_Task
 *  Chạy liên tục, kiểm tra trạng thái 4 switch mỗi 100ms.
 *  Khi phát hiện switch thay đổi → thực hiện chuyển đổi chế độ ngay lập tức.
 * ───────────────────────────────────────────────────────── */
void Switch_Monitor_Task(void *pvParameters)
{
    // Trạng thái switch tại lần đọc trước
    bool prev_sw1 = switch_is_on(SW1_CONFIG_PIN);
    bool prev_sw2 = switch_is_on(SW2_DAC_I2S_PIN);
    bool prev_sw3 = switch_is_on(SW3_BT_PIN);
    bool prev_sw4 = switch_is_on(SW4_RESERVED_PIN);

    ESP_LOGI(TAG, "[SWITCH MONITOR] Initial: SW1(Config)=%d SW2(DAC)=%d SW3(BT)=%d SW4=%d",
             prev_sw1, prev_sw2, prev_sw3, prev_sw4);

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(100));

        bool cur_sw1 = switch_is_on(SW1_CONFIG_PIN);
        bool cur_sw2 = switch_is_on(SW2_DAC_I2S_PIN);
        bool cur_sw3 = switch_is_on(SW3_BT_PIN);
        bool cur_sw4 = switch_is_on(SW4_RESERVED_PIN);

        bool changed = (cur_sw1 != prev_sw1) ||
                       (cur_sw2 != prev_sw2) ||
                       (cur_sw3 != prev_sw3) ||
                       (cur_sw4 != prev_sw4);

        if (changed)
        {
            if (cur_sw1 != prev_sw1)
            {
                /* Debounce SW1: cần ổn định 500ms trước khi restart (tránh nhiễu khi gạt SW2/SW3) */
                vTaskDelay(pdMS_TO_TICKS(500));
                if (switch_is_on(SW1_CONFIG_PIN) != cur_sw1) {
                    continue; /* nhiễu, bỏ qua */
                }
                ESP_LOGW(TAG, "[SWITCH] SW1 (GPIO%d, Config/AP) : %s → %s. Restarting system to apply WiFi mode change...",
                         SW1_CONFIG_PIN,
                         prev_sw1 ? "ON" : "OFF",
                         cur_sw1  ? "ON" : "OFF");
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }

            if (cur_sw2 != prev_sw2 || cur_sw3 != prev_sw3)
            {
                if (!sys_boot_complete) {
                    ESP_LOGW(TAG, "[SWITCH] Ignore SW2/SW3 during boot (SW2=%d SW3=%d)", cur_sw2, cur_sw3);
                    prev_sw2 = cur_sw2;
                    prev_sw3 = cur_sw3;
                    prev_sw4 = cur_sw4;
                    continue;
                }

                ESP_LOGW(TAG, "[SWITCH] Audio switch change detected: SW2(DAC):%d→%d, SW3(BT):%d→%d",
                         prev_sw2, cur_sw2, prev_sw3, cur_sw3);

                // Stop active playbacks
                Stop_Alarm();

                bool target_dac = cur_sw2;
                bool target_bt  = (!cur_sw2 && cur_sw3);

                // 1. Transition FROM current active mode
                if (sys_is_dac_i2s_mode)
                {
                    if (target_bt || (!target_dac && !target_bt))
                    {
                        ESP_LOGI(TAG, "Stopping DAC I2S Mode...");
                        sys_is_dac_i2s_mode = false; // signals DAC_I2S_Task to exit
                        // Wait for DAC_I2S_Task to deinitialize and exit
                        while (DAC_I2S_Task_Handle != NULL) {
                            vTaskDelay(pdMS_TO_TICKS(20));
                        }
                        ESP_LOGI(TAG, "DAC I2S Mode stopped successfully.");
                    }
                }
                else if (sys_is_bt_mode)
                {
                    if (target_dac || (!target_dac && !target_bt))
                    {
                        ESP_LOGI(TAG, "Stopping Bluetooth Mode...");
                        sys_is_bt_mode = false;
                        user_bluetooth_deinit();
                        ESP_LOGI(TAG, "Bluetooth Mode stopped successfully.");
                    }
                }

                // 2. Transition TO target mode
                if (target_dac)
                {
                    if (!sys_is_dac_i2s_mode)
                    {
                        ESP_LOGI(TAG, "Starting DAC I2S Mode...");
                        sys_is_dac_i2s_mode = true;
                        xTaskCreatePinnedToCore(DAC_I2S_Task, "DAC_I2S_Task", 4096, NULL, 5, &DAC_I2S_Task_Handle, 1);
                    }
                }
                else if (target_bt)
                {
                    if (!sys_is_bt_mode)
                    {
                        ESP_LOGI(TAG, "Starting Bluetooth Mode...");
                        sys_is_bt_mode = true;
                        user_bluetooth_init();
                    }
                }
                else
                {
                    ESP_LOGI(TAG, "No audio mode selected.");
                }
            }

            if (cur_sw4 != prev_sw4)
            {
                ESP_LOGW(TAG, "[SWITCH] SW4 (GPIO%d, Reserved) : %s → %s",
                         SW4_RESERVED_PIN,
                         prev_sw4 ? "ON" : "OFF",
                         cur_sw4  ? "ON" : "OFF");
            }

            prev_sw1 = cur_sw1;
            prev_sw2 = cur_sw2;
            prev_sw3 = cur_sw3;
            prev_sw4 = cur_sw4;
        }
    }
}

/* Khởi chạy 3 tác vụ LED + 1 tác vụ giám sát switch */
static void start_led_tasks(void)
{
    xTaskCreatePinnedToCore(Led1_Config_Task,    "LED1 Config",   2048, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(Led2_Wifi_Task,      "LED2 WiFi",     2048, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(Led3_Audio_Task,     "LED3 Audio",    2048, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(Switch_Monitor_Task, "Switch Monitor", 2048, NULL, 1, NULL, 1);
}

void app_main(void)
{
  ESP_LOGI(TAG, "=== app_main start ===");

  /* Bước 1: Khởi tạo NVS Flash */
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  /* Xác nhận firmware sớm (cả Config và Normal mode) để tránh OTA rollback */
  User_Ota_Mark_Valid();

  /* Bước 2: Khởi tạo switch và LED */
  switches_init();
  leds_init();

  /* Bước 3: Đọc trạng thái switch phần cứng để quyết định chế độ
   *   SW1 (GPIO13) ON (LOW) → Config Mode
   *   SW2 (GPIO12) ON (LOW) → DAC I2S Mode
   *   SW3 (GPIO14) ON (LOW) → Bluetooth Mode
   * Chú ý: ưu tiên SW1 (Config) trước; SW2 và SW3 chỉ có hiệu lực
   * khi SW1 ở vị trí OFF (Normal Mode).
   */
  bool sw1_config = switch_is_on(SW1_CONFIG_PIN);
  bool sw2_dac    = switch_is_on(SW2_DAC_I2S_PIN);
  bool sw3_bt     = switch_is_on(SW3_BT_PIN);

  ESP_LOGI(TAG, "Switch states → SW1(Config):%d  SW2(DAC):%d  SW3(BT):%d",
           sw1_config, sw2_dac, sw3_bt);

  /* ═══════════════════════════════════════════
   *  CHẾ ĐỘ CẤU HÌNH (CONFIG MODE)
   *  Khi SW1 ở vị trí ON hoặc chưa có cấu hình WiFi
   * ═══════════════════════════════════════════ */
  char nvs_ssid[64] = {0}, nvs_pass[64] = {0};
  bool has_wifi = (load_wifi_from_nvs(nvs_ssid, sizeof(nvs_ssid), nvs_pass, sizeof(nvs_pass)) && strlen(nvs_ssid) > 0);

  if (sw1_config || !has_wifi)
  {
      ESP_LOGI(TAG, "================================");
      ESP_LOGI(TAG, " ENTERING CONFIG MODE");
      if (sw1_config)  ESP_LOGI(TAG, "  Reason: SW1 switch ON");
      if (!has_wifi)   ESP_LOGI(TAG, "  Reason: No WiFi credentials");
      ESP_LOGI(TAG, "  Web: http://192.168.4.1  WiFi: ESP32_Speaker_Setup");
      ESP_LOGI(TAG, "================================");

      sys_is_in_config_mode = true;
      wifi_init_config_mode();

      /* Khởi chạy 3 tác vụ LED */
      start_led_tasks();

      if (xTaskCreatePinnedToCore(Http_Server_Task, "Http Server Task", 4 * 4096, NULL, 2, &Http_Server_Task_Handle, 1) == pdPASS) {
          ESP_LOGI(TAG, "Create HTTP server task successfully");
      }

      /* Config mode: chạy vĩnh viễn cho đến khi người dùng tắt SW1 và reset */
      while (1) {
          vTaskDelay(pdMS_TO_TICKS(1000));
      }
  }

  /* ═══════════════════════════════════════════
   *  CHẾ ĐỘ BÌNH THƯỜNG (NORMAL MODE)
   * ═══════════════════════════════════════════ */
  else
  {
      ESP_LOGI(TAG, "================================");
      ESP_LOGI(TAG, " ENTERING NORMAL MODE (STA)   ");
      ESP_LOGI(TAG, "================================");

      const esp_app_desc_t *app_desc = esp_app_get_description();
      ESP_LOGI(TAG, "RUNNING VERSION: %s", app_desc->version);

      /* Chọn chế độ âm thanh theo switch phần cứng:
       *   SW2 ON              → DAC I2S (ưu tiên kể cả khi SW3 cũng ON)
       *   SW3 ON (SW2 OFF)    → Bluetooth A2DP
       *   Cả hai OFF          → Không chọn chế độ nào, không khởi tạo âm thanh
       */
      if (sw2_dac)
      {
          sys_is_dac_i2s_mode = true;
          sys_is_bt_mode      = false;
          ESP_LOGI(TAG, "================================");
          if (sw3_bt) {
              ESP_LOGI(TAG, "Audio Mode: DAC I2S (PCM5102) [SW2+SW3 ON → SW2 priority]");
          } else {
              ESP_LOGI(TAG, "Audio Mode: DAC I2S (PCM5102) [SW2 ON]");
          }
          ESP_LOGI(TAG, "================================");
          xTaskCreatePinnedToCore(DAC_I2S_Task, "DAC_I2S_Task", 4096, NULL, 5, &DAC_I2S_Task_Handle, 1);
      }
      else if (sw3_bt)
      {
          sys_is_bt_mode      = true;
          sys_is_dac_i2s_mode = false;
          ESP_LOGI(TAG, "================================");
          ESP_LOGI(TAG, "Audio Mode: BLUETOOTH A2DP [SW3 ON]");
          ESP_LOGI(TAG, "================================");
      }
      else
      {
          /* Cả SW2 và SW3 đều OFF: không khởi tạo âm thanh */
          sys_is_dac_i2s_mode = false;
          sys_is_bt_mode      = false;
          ESP_LOGW(TAG, "SW2 and SW3 both OFF: no audio mode selected.");
      }

      /* Initialize OTA event group early (task created lazily on update command) */
      User_Ota_Init();
      log_heap("boot");

      wifi_init_normal_mode();
      User_System_Init();
      log_heap("after wifi");

      /* Queues need INTERNAL RAM — create before BT eats the heap */
      audio_queues_init();

      /* Khởi chạy 3 tác vụ LED */
      start_led_tasks();

      /* Bell task nhẹ — tạo sớm khi heap còn nhiều (tránh fail sau Azure/BT) */
      if (xTaskCreatePinnedToCore(Bell_Task, "Bell Task", 3072, NULL, 4, NULL, 1) == pdPASS) {
          ESP_LOGI(TAG, "Create Bell task successfully");
      } else {
          ESP_LOGE(TAG, "Create Bell task fail");
      }

      if (!Sys_Info.isWifiConnected) {
          ESP_LOGW(TAG, "Waiting for STA WiFi...");
          while (!Sys_Info.isWifiConnected) {
              vTaskDelay(pdMS_TO_TICKS(1000));
          }
      }

      if (xTaskCreatePinnedToCore(Timer_Task, "Timer Task", 1 * 4096, NULL, 2, &Timer_Task_Handle, 1) == pdPASS) {
          ESP_LOGI(TAG, "Create Timer task successfully");
      }

      /* NTP: Timer task sync; app_main chờ tối đa 8s rồi boot tiếp */
      {
          const int ntp_deadline_ms = 8000;
          int waited = 0;
          while (!Sys_Info.isTimeSync && waited < ntp_deadline_ms) {
              vTaskDelay(pdMS_TO_TICKS(200));
              waited += 200;
          }
          if (!Sys_Info.isTimeSync) {
              ESP_LOGW(TAG, "NTP not ready after %d ms — continue boot (sync in background)", ntp_deadline_ms);
          }
      }
      log_heap("after ntp");

      my_spiffc_init();
      log_heap("after spiffs");

      if (xTaskCreatePinnedToCore(SDCard_Task, "SDCard Task", 3 * 4096, NULL, 3, &SDCard_Task_Handle, 1) == pdPASS) {
          ESP_LOGI(TAG, "Create SDCard task successfully");
      }
      log_heap("after sdcard task");

      /* Azure trước — giữ cloud; BT trước HTTP để còn DRAM cho Bluedroid */
      if (xTaskCreatePinnedToCore(Azure_Task, "Azure Task", 5 * 4096, NULL, 5, &Azure_Task_Handle, 0) == pdPASS) {
          ESP_LOGI(TAG, "Create Azure task successfully");
      }

      while (!IoTHubHandle.isAzureInitialized) {
          vTaskDelay(pdMS_TO_TICKS(1000));
      }
      log_heap("after azure");

      if (sys_is_bt_mode) {
          ESP_LOGI(TAG, "Starting Bluetooth after Azure (before HTTP)...");
          user_bluetooth_init();
          vTaskDelay(pdMS_TO_TICKS(300));
          log_heap("after bt init");
      }

      /* HTTP sau BT — task stack nhỏ hơn để còn internal heap */
      if (xTaskCreatePinnedToCore(Http_Server_Task, "Http Server Task", 8192, NULL, 2, &Http_Server_Task_Handle, 1) == pdPASS) {
          ESP_LOGI(TAG, "Create HTTP server task successfully");
      } else {
          ESP_LOGE(TAG, "Create HTTP server task fail");
      }
      log_heap("after http");

      if (xTaskCreatePinnedToCore(sd_read_task, "SD Read Task", 3 * 4096, NULL, 5, NULL, 0) == pdPASS)
      {
          ESP_LOGI(TAG, "Create SD Read task successfully");
      }
      else
      {
          ESP_LOGE(TAG, "Create SD Read task fail");
      }

      sys_boot_complete = true;
      log_heap("boot complete");
  }
}
