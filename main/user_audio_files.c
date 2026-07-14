/*******************************************************************************
 * FILE: user_audio_files.c
 * Manage WAV files on physical SD; download from HTTPS (Azure Blob SAS).
 ******************************************************************************/

#include "user_audio_files.h"
#include "user_sdcard.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"

static const char *TAG = "AUDIO_FILES";

volatile bool bAudioDownloadPending = false;
char g_audio_dl_url[AUDIO_FILE_URL_MAX] = {0};
char g_audio_dl_name[AUDIO_FILE_NAME_MAX] = {0};

bool audio_files_sd_ready(void)
{
    return isSdCardReady;
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
    char tmp_path[AUDIO_PATH_MAX + 8];
    if (!build_path(path, sizeof(path), safe)) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 60000,
        .buffer_size = 4096,
        .keep_alive_enable = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP status %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot create %s", tmp_path);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char buf[2048];
    int total = 0;
    while (1) {
        int r = esp_http_client_read(client, buf, sizeof(buf));
        if (r < 0) {
            ESP_LOGE(TAG, "HTTP read error");
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            err = ESP_OK;
            break;
        }
        if (fwrite(buf, 1, r, f) != (size_t)r) {
            ESP_LOGE(TAG, "fwrite failed");
            err = ESP_FAIL;
            break;
        }
        total += r;
    }

    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        unlink(tmp_path);
        return err;
    }

    unlink(path); /* replace existing */
    if (rename(tmp_path, path) != 0) {
        ESP_LOGE(TAG, "rename failed %s -> %s", tmp_path, path);
        unlink(tmp_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Downloaded %d bytes (content_len=%d) -> %s", total, content_len, path);
    return ESP_OK;
}

static void audio_dl_worker(void *pv)
{
    (void)pv;
    ESP_LOGI(TAG, "Worker start: %s", g_audio_dl_name);
    esp_err_t e = audio_files_download(g_audio_dl_url, g_audio_dl_name);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Worker failed: %s", esp_err_to_name(e));
    } else {
        ESP_LOGI(TAG, "Worker OK: /sdcard/%s", g_audio_dl_name);
    }
    bAudioDownloadPending = false;
    vTaskDelete(NULL);
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

    if (xTaskCreate(audio_dl_worker, "audio_dl", 8192, NULL, 5, NULL) != pdPASS) {
        bAudioDownloadPending = false;
        ESP_LOGE(TAG, "Failed to create download task");
        return false;
    }
    ESP_LOGI(TAG, "Download queued: %s", g_audio_dl_name);
    return true;
}
