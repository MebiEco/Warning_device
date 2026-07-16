/*******************************************************************************
 * FILE: user_audio_files.c
 * Manage WAV files on physical SD; download from HTTPS (Azure Blob SAS).
 ******************************************************************************/

#include "user_audio_files.h"
#include "user_sdcard.h"
#include "user_bluetooth.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"

static const char *TAG = "AUDIO_FILES";

volatile bool bAudioDownloadPending = false;
volatile bool bAudioDownloadActive = false;
char g_audio_dl_url[AUDIO_FILE_URL_MAX] = {0};
char g_audio_dl_name[AUDIO_FILE_NAME_MAX] = {0};

bool audio_files_is_download_busy(void)
{
    return bAudioDownloadPending || bAudioDownloadActive;
}

bool audio_files_wait_idle(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (audio_files_is_download_busy()) {
        if (waited >= timeout_ms) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    return true;
}

bool audio_files_sd_ready(void)
{
    /* List/play work on SD or flash fallback; both mount at /sdcard */
    return isCardInited;
}

bool audio_files_sanitize_name(const char *in, char *out, size_t out_len)
{
    if (!in || !out || out_len < 2) {
        return false;
    }

    /* strip path separators if client sent a path */
    const char *base = strrchr(in, '/');
    if (base) {
        in = base + 1;
    }
    base = strrchr(in, '\\');
    if (base) {
        in = base + 1;
    }

    if (in[0] == '\0' || strcmp(in, ".") == 0 || strcmp(in, "..") == 0) {
        return false;
    }

    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 1 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '.' || c == '_' || c == '-') {
            out[j++] = (char)c;
        } else {
            return false;
        }
    }
    out[j] = '\0';
    if (j == 0) {
        return false;
    }
    if (strstr(out, "..") != NULL) {
        return false;
    }
    return true;
}

#define AUDIO_PATH_MAX  (sizeof(MOUNT_POINT) + AUDIO_FILE_NAME_MAX + 2)

static bool build_path(char *path, size_t path_len, const char *name)
{
    if (!path || !name || path_len < 8) {
        return false;
    }
    /* Cap to our max filename so snprintf cannot truncate unexpectedly */
    size_t nlen = strnlen(name, AUDIO_FILE_NAME_MAX + 1);
    if (nlen == 0 || nlen > AUDIO_FILE_NAME_MAX) {
        return false;
    }
    int n = snprintf(path, path_len, "%s/%.*s", MOUNT_POINT, (int)nlen, name);
    return (n > 0 && (size_t)n < path_len);
}

char *audio_files_list_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return NULL;
    }

    if (!audio_files_sd_ready()) {
        char *s = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        return s;
    }

    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        char *s = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        return s;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char path[AUDIO_PATH_MAX];
        if (!build_path(path, sizeof(path), ent->d_name)) {
            continue;
        }
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            continue;
        }
        cJSON_AddStringToObject(obj, "name", ent->d_name);
        cJSON_AddNumberToObject(obj, "size", (double)st.st_size);
        cJSON_AddItemToArray(arr, obj);
    }
    closedir(dir);

    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s;
}

esp_err_t audio_files_delete(const char *filename)
{
    if (!audio_files_sd_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    char safe[AUDIO_FILE_NAME_MAX];
    if (!audio_files_sanitize_name(filename, safe, sizeof(safe))) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[AUDIO_PATH_MAX];
    if (!build_path(path, sizeof(path), safe)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (unlink(path) != 0) {
        ESP_LOGE(TAG, "unlink failed: %s", path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Deleted %s", path);
    return ESP_OK;
}

esp_err_t audio_files_download(const char *url, const char *filename)
{
    if (!audio_files_sd_ready()) {
        ESP_LOGE(TAG, "SD card not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (!url || strncmp(url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "URL must be https://");
        return ESP_ERR_INVALID_ARG;
    }

    char safe[AUDIO_FILE_NAME_MAX];
    if (!audio_files_sanitize_name(filename, safe, sizeof(safe))) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[AUDIO_PATH_MAX];
    char tmp_path[AUDIO_PATH_MAX];
    if (!build_path(path, sizeof(path), safe)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* FATFS_LFN_NONE = 8.3 only — "121.wav.tmp" is invalid (errno 22). Use "121.tmp". */
    {
        char stem[AUDIO_FILE_NAME_MAX];
        char ext[4] = {0};
        strncpy(stem, safe, sizeof(stem) - 1);
        stem[sizeof(stem) - 1] = '\0';
        char *dot = strrchr(stem, '.');
        if (dot) {
            if (strlen(dot + 1) == 0 || strlen(dot + 1) > 3) {
                ESP_LOGE(TAG, "Extension must be 1-3 chars (8.3): %s", safe);
                return ESP_ERR_INVALID_ARG;
            }
            strncpy(ext, dot + 1, 3);
            *dot = '\0';
        } else {
            ESP_LOGE(TAG, "Filename needs extension (e.g. .wav): %s", safe);
            return ESP_ERR_INVALID_ARG;
        }
        if (stem[0] == '\0' || strlen(stem) > 8) {
            ESP_LOGE(TAG, "Base name must be 1-8 chars (8.3): %s", safe);
            return ESP_ERR_INVALID_ARG;
        }
        (void)ext;
        int n = snprintf(tmp_path, sizeof(tmp_path), "%s/%s.tmp", MOUNT_POINT, stem);
        if (n <= 0 || (size_t)n >= sizeof(tmp_path)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* Pause A2DP first — TLS + SD under BT stream trips TASK_WDT (IDLE0 starved) */
    user_bt_pause_for_wifi();

    /* Wait for a usable DMA free block (SDSPI needs it) */
    for (int i = 0; i < 30; i++) {
        size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
        size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
        if (dma_largest >= 4096 && dma_free >= 12288) {
            break;
        }
        ESP_LOGW(TAG, "Waiting DMA for SD write (free=%u largest=%u)",
                 (unsigned)dma_free, (unsigned)dma_largest);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* Open SD file BEFORE TLS — fail early if diskio has no DMA */
    unlink(tmp_path);
    FILE *f = NULL;
    for (int attempt = 0; attempt < 5 && !f; attempt++) {
        f = fopen(tmp_path, "wb");
        if (!f) {
            ESP_LOGW(TAG, "fopen %s failed errno=%d (try %d)", tmp_path, errno, attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
    if (!f) {
        ESP_LOGE(TAG, "Cannot create %s errno=%d", tmp_path, errno);
        user_bt_resume_after_wifi();
        return ESP_FAIL;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 90000,
        .buffer_size = 2048,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        fclose(f);
        unlink(tmp_path);
        user_bt_resume_after_wifi();
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        fclose(f);
        unlink(tmp_path);
        user_bt_resume_after_wifi();
        return err;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP status %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        fclose(f);
        unlink(tmp_path);
        user_bt_resume_after_wifi();
        return ESP_FAIL;
    }

    const size_t buf_sz = 2048;
    char *buf = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(buf_sz);
    }
    if (!buf) {
        ESP_LOGE(TAG, "No buffer for download");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        fclose(f);
        unlink(tmp_path);
        user_bt_resume_after_wifi();
        return ESP_ERR_NO_MEM;
    }

    int total = 0;
    int chunk = 0;
    err = ESP_OK;
    while (1) {
        int r = esp_http_client_read(client, buf, buf_sz);
        if (r < 0) {
            ESP_LOGE(TAG, "HTTP read error");
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            break;
        }
        if (fwrite(buf, 1, r, f) != (size_t)r) {
            ESP_LOGE(TAG, "fwrite failed errno=%d", errno);
            err = ESP_FAIL;
            break;
        }
        total += r;
        /* Yield so IDLE0 / WiFi can run (avoids TASK_WDT during long TLS reads) */
        if ((++chunk & 7) == 0) {
            vTaskDelay(1);
        }
    }

    free(buf);
    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    user_bt_resume_after_wifi();

    if (err != ESP_OK) {
        unlink(tmp_path);
        return err;
    }

    unlink(path); /* replace existing */
    if (rename(tmp_path, path) != 0) {
        ESP_LOGE(TAG, "rename failed %s -> %s errno=%d", tmp_path, path, errno);
        unlink(tmp_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Downloaded %d bytes (content_len=%d) -> %s", total, content_len, path);
    return ESP_OK;
}

void audio_files_run_pending_download(void)
{
    if (!bAudioDownloadPending) {
        return;
    }

    bAudioDownloadActive = true;
    ESP_LOGI(TAG, "Azure-task download start: %s", g_audio_dl_name);
    esp_err_t e = audio_files_download(g_audio_dl_url, g_audio_dl_name);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Download failed: %s", esp_err_to_name(e));
    } else {
        ESP_LOGI(TAG, "Download OK: /sdcard/%s", g_audio_dl_name);
    }
    bAudioDownloadPending = false;
    bAudioDownloadActive = false;
}

bool audio_files_request_download(const char *url, const char *filename)
{
    char safe[AUDIO_FILE_NAME_MAX];
    if (!url || !filename) {
        return false;
    }
    if (bAudioDownloadPending) {
        ESP_LOGW(TAG, "Download already pending");
        return false;
    }
    if (!audio_files_sanitize_name(filename, safe, sizeof(safe))) {
        return false;
    }
    if (strncmp(url, "https://", 8) != 0) {
        return false;
    }
    if (!audio_files_sd_ready()) {
        return false;
    }

    memset(g_audio_dl_url, 0, sizeof(g_audio_dl_url));
    memset(g_audio_dl_name, 0, sizeof(g_audio_dl_name));
    strncpy(g_audio_dl_url, url, sizeof(g_audio_dl_url) - 1);
    strncpy(g_audio_dl_name, safe, sizeof(g_audio_dl_name) - 1);
    bAudioDownloadPending = true;

    /* Run on User_Azure_Task — avoid xTaskCreate (fails when internal heap is fragmented). */
    ESP_LOGI(TAG, "Download queued for Azure task: %s", g_audio_dl_name);
    return true;
}
