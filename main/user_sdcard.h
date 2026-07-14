#pragma once
#include "stdbool.h"
#define MOUNT_POINT "/sdcard"

/** true if /sdcard is mounted (SD or flash fallback) */
extern bool isCardInited;
/** true only when physical SD card mounted (required for Azure/web file ops) */
extern bool isSdCardReady;

void User_SDCard_Task(void);
