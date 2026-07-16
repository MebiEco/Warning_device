/*******************************************************************************
 * FILE: user_ota.c
 * MÔ TẢ: OTA Firmware Update qua HTTPS (Azure Blob Storage)
 *
 * LUỒNG HOẠT ĐỘNG:
 *   1. Khởi động: gọi User_Ota_Mark_Valid() để xác nhận app hiện tại OK
 *      → Nếu không gọi, bootloader sẽ rollback về firmware trước sau khi reboot
 *   2. Nhận lệnh OTA từ Azure (Code 501): Azure set OTA_WAIT_BIT
 *   3. User_Ota_Task() chờ bit → download firmware mới → flash vào ota_1 → reboot
 *   4. Bootloader boot từ ota_1 (firmware mới)
 *   5. Nếu firmware mới OK → gọi User_Ota_Mark_Valid() → xác nhận, giữ ota_1
 *      Nếu firmware mới crash → bootloader tự rollback về ota_0 (firmware cũ)
 ******************************************************************************/

#include "user_ota.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include <string.h>

#include "user_bluetooth.h"

#define OTA_TAG "OTA"

EventGroupHandle_t otaEventGroup = NULL;
char g_ota_update_url[256]       = {0};

static bool s_ota_use_auth_header = true;

/*******************************************************************************
 * HÀM: User_Ota_Mark_Valid
 * MÔ TẢ: Xác nhận firmware hiện tại hoạt động bình thường.
 *        PHẢI gọi hàm này sau khi hệ thống đã khởi động ổn định (WiFi OK, Azure OK).
 *        Nếu không gọi → sau khi reboot, bootloader sẽ rollback về firmware trước.
 ******************************************************************************/
void User_Ota_Mark_Valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK)
    {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY)
        {
            ESP_LOGI(OTA_TAG, "Firmware pending verify — marking as VALID.");
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err == ESP_OK)
            {
                ESP_LOGI(OTA_TAG, "Firmware marked VALID. Rollback cancelled.");
            }
            else
            {
                ESP_LOGE(OTA_TAG, "Failed to mark firmware valid: %s", esp_err_to_name(err));
            }
        }
        else
        {
            /* Factory hoặc firmware đã được xác nhận trước đó */
            ESP_LOGD(OTA_TAG, "Running partition state: %d (no action needed)", ota_state);
        }
    }

    /* Log thông tin partition đang chạy */
    ESP_LOGI(OTA_TAG, "Running partition: label='%s', offset=0x%08lx, size=0x%08lx",
             running->label, running->address, running->size);
}

/*******************************************************************************
 * EVENT HANDLER: HTTP event cho OTA HTTPS client
 ******************************************************************************/
static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_CONNECTED)
    {
        if (s_ota_use_auth_header)
        {
            /* Dùng Authorization header nếu URL không chứa SAS token (sig=) */
            esp_http_client_set_header(evt->client, "Authorization",
                "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"
                ".eyJUZW5hbnRDb2RlIjoicHZvaWwiLCJodHRwOi8vc2NoZW1hcy5taWNyb3NvZnQuY29tL3dzLzIwMDgv"
                "MDYvaWRlbnRpdHkvY2xhaW1zL3JvbGUiOiJEZXZpY2UiLCJVc2VyTmFtZSI6InBlY28iLCJuYmYiOjE2"
                "NDQ1NTIxOTcsImV4cCI6MTcwNzY2NjAxNywiaXNzIjoiaHR0cDovL3NtYXJ0cGV0cm8uaW8vIiwiYXVkIj"
                "oiU21hcnRQZXRybyJ9.03hQ3zdz3YJO-y8lfYV805qhapYts1iwdHkwVR-skms");
        }
    }
    return ESP_OK;
}

/*******************************************************************************
 * HÀM NỘI BỘ: Xây URL đầy đủ nếu chỉ truyền vào tên file/path
 ******************************************************************************/
static bool prvBuildOtaUrl(const char *updateFileName, char *out, size_t out_len)
{
    if (updateFileName == NULL || out == NULL || out_len == 0)
    {
        return false;
    }

    static const char *base_url = "https://shrimpiotdblobs.blob.core.windows.net";

    if (updateFileName[0] == '/')
    {
        return (snprintf(out, out_len, "%s%s", base_url, updateFileName) > 0);
    }

    return (snprintf(out, out_len, "%s/%s", base_url, updateFileName) > 0);
}

/*******************************************************************************
 * HÀM: update_firmware
 * MÔ TẢ: Download và flash firmware mới vào OTA partition.
 *        Nếu thành công → esp_restart() (không return).
 *        Nếu thất bại → return ESP_FAIL (caller quyết định có reboot không).
 ******************************************************************************/
esp_err_t update_firmware(const char *updateFileName)
{
    if (updateFileName == NULL || strlen(updateFileName) == 0)
    {
        ESP_LOGE(OTA_TAG, "Invalid update file name");
        return ESP_FAIL;
    }

    /* Xây URL đầy đủ */
    char updateUrl[256] = {0};
    if ((strncmp(updateFileName, "http://", 7) == 0) ||
        (strncmp(updateFileName, "https://", 8) == 0))
    {
        if (snprintf(updateUrl, sizeof(updateUrl), "%s", updateFileName) <= 0)
        {
            return ESP_FAIL;
        }
    }
    else if (!prvBuildOtaUrl(updateFileName, updateUrl, sizeof(updateUrl)))
    {
        ESP_LOGE(OTA_TAG, "Failed to build OTA URL from: %s", updateFileName);
        return ESP_FAIL;
    }

    /* Nếu URL có SAS token (sig=) thì không cần Authorization header */
    s_ota_use_auth_header = (strstr(updateUrl, "sig=") == NULL);

    ESP_LOGI(OTA_TAG, "OTA URL: %s", updateUrl);
    ESP_LOGI(OTA_TAG, "Auth header: %s", s_ota_use_auth_header ? "YES" : "NO (SAS token)");

    esp_http_client_config_t config = {
        .url                         = updateUrl,
        .event_handler               = _http_event_handler,
        .crt_bundle_attach           = esp_crt_bundle_attach,
        .keep_alive_enable           = true,   /* Giữ TCP trong suốt download */
        .buffer_size                 = 4096,   /* HTTP recv buffer */
        .buffer_size_tx              = 2048,   /* HTTP send buffer */
        .timeout_ms                  = 30000,  /* 30s timeout tránh hang */
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    ESP_LOGI(OTA_TAG, "Starting OTA download...");
    esp_err_t ret = esp_https_ota(&ota_config);

    if (ret == ESP_OK)
    {
        ESP_LOGI(OTA_TAG, "OTA download & flash succeeded. Rebooting to new firmware...");
        /* Reboot → bootloader sẽ chạy firmware mới ở OTA partition kia */
        esp_restart();
        /* Không bao giờ đến đây */
    }
    else
    {
        ESP_LOGE(OTA_TAG, "OTA failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

/*******************************************************************************
 * TASK: User_Ota_Task
 * MÔ TẢ: Daemon task, chờ tín hiệu OTA từ Azure_Handle_Direct_Method_Data().
 *        Khi nhận được lệnh → chờ 3s (Azure Task dọn heap) → bắt đầu update.
 ******************************************************************************/
void User_Ota_Task(void)
{
    // otaEventGroup is now initialized in main.c
    if (otaEventGroup == NULL)
    {
        ESP_LOGE(OTA_TAG, "OTA event group is NULL! OTA disabled.");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(OTA_TAG, "OTA Task started. Waiting for update command...");

    while (1)
    {
        EventBits_t bits = xEventGroupWaitBits(
            otaEventGroup,
            OTA_WAIT_BIT,
            pdTRUE,         /* xClearOnExit: tự clear bit sau khi nhận */
            pdFALSE,
            portMAX_DELAY
        );

        if (bits & OTA_WAIT_BIT)
        {
            ESP_LOGW(OTA_TAG, "OTA command received! Preparing system for update...");
            ESP_LOGI(OTA_TAG, "Target URL: %s", g_ota_update_url);
            
            /* Stop Bluetooth to free HEAP and CPU */
            ESP_LOGW(OTA_TAG, "Stopping Bluetooth stack to free resources...");
            user_bluetooth_deinit();
            ESP_LOGI(OTA_TAG, "Bluetooth stopped successfully.");

            ESP_LOGI(OTA_TAG, "Waiting 3s for system to stabilize before download...");
            vTaskDelay(pdMS_TO_TICKS(3000));

            ESP_LOGI(OTA_TAG, "Starting firmware download: %s", g_ota_update_url);
            esp_err_t ret = update_firmware(g_ota_update_url);

            /* Nếu update_firmware() return (tức là thất bại), reboot để tránh treo */
            if (ret != ESP_OK)
            {
                ESP_LOGE(OTA_TAG, "OTA failed. Rebooting to restore normal operation.");
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }
        }
    }
}

