#pragma once

#include "esp_https_ota.h"
// #include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

/* Bit báo hiệu có lệnh OTA mới từ Azure */
#define OTA_WAIT_BIT   (1 << 0)

/* URL của firmware sẽ được download (ghi bởi Azure handler, đọc bởi OTA task) */
extern char g_ota_update_url[256];

/* Event group để giao tiếp giữa Azure handler và OTA task */
extern EventGroupHandle_t otaEventGroup;

/* Azure task chạy download sau khi prepare (tránh tạo thêm task khi heap phân mảnh) */
extern volatile bool bOtaPendingDownload;

/*
 * Xác nhận firmware hiện tại hoạt động tốt — HỦY rollback.
 * Gọi hàm này sau khi hệ thống đã kết nối WiFi + Azure thành công.
 * Nếu không gọi → bootloader sẽ tự rollback về firmware trước sau khi reboot.
 */
void User_Ota_Mark_Valid(void);

/* Khởi tạo event group OTA (nhẹ, gọi lúc boot) */
void User_Ota_Init(void);

/* Tạo OTA task khi nhận lệnh update (tiết kiệm RAM lúc boot) */
void User_Ota_Ensure_Task(void);

/* Azure task gọi trước khi download */
void User_Ota_Prepare_System(void);

/* Queue OTA từ Direct Method handler (không block process loop) */
bool User_Ota_Request_Update(const char *url);

/* Daemon task: chờ lệnh OTA từ Azure, download + flash firmware mới */
void User_Ota_Task(void *pvParameters);

/* Hàm download và flash firmware từ URL (gọi nội bộ bởi User_Ota_Task) */
esp_err_t update_firmware(const char *updateFileName);
