#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

#define AUDIO_FILE_NAME_MAX  64
#define AUDIO_FILE_URL_MAX   384

/** True only when physical SD card is mounted (not flash fallback). */
bool audio_files_sd_ready(void);

/** Sanitize user filename into out; returns false if invalid. */
bool audio_files_sanitize_name(const char *in, char *out, size_t out_len);

/** Build JSON array of {name,size} on /sdcard. Caller frees with free(). */
char *audio_files_list_json(void);

/** Delete /sdcard/<name>. */
esp_err_t audio_files_delete(const char *filename);

/**
 * Download HTTPS URL to /sdcard/<filename> (temp then rename).
 * Requires SD ready. Blocks until done.
 */
esp_err_t audio_files_download(const char *url, const char *filename);

/** Queue download for Azure task (non-blocking). */
bool audio_files_request_download(const char *url, const char *filename);

extern volatile bool bAudioDownloadPending;
extern char g_audio_dl_url[AUDIO_FILE_URL_MAX];
extern char g_audio_dl_name[AUDIO_FILE_NAME_MAX];
