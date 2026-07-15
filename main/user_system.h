#pragma once

#include "stdbool.h"
#include "stdint.h"
#include "time.h"


// #define CRC32_BYPASS        1

#define START_OF_FRAME 0x02
#define END_OF_FRAME 0x03

#define SYS_SERVER_IP_DEFAULT "192.168.1.100"
#define SYS_SERVER_PORT_DEFAULT 3000
// #define SYS_WIFI_SSID_DEFAULT "MebiOneIOT"
// #define SYS_WIFI_PASS_DEFAULT "MebiOne@123"

// #define SYS_IOT_HUB_HOST_NAME_DEFAULT "VuIoTHub.azure-devices.net"
// #define SYS_IOT_HUB_DEVICE_ID_DEFAULT "VuDevice"
// #define SYS_IOT_HUB_SYMMETRIC_KEY_DEFAULT                                      
//   "xOw6BICGo79moY00yntL7ZTQ0eRZD3sICfOe+29qRr0="

/* Define cmd code — Direct Method "Control" */
#define CMD_CODE_LOA                 100  /* Play / stop alarm WAV */
#define CMD_CODE_CONTROL_ON_OFF      101  /* Bell / siren GPIO */
#define CMD_CODE_CONTROL_SCHEDULE    102  /* Reserved */
#define CMD_CODE_UPDATE_FIRMWARE     501  /* OTA */
#define CMD_CODE_AUDIO_DOWNLOAD      600  /* HTTPS → SD */
#define CMD_CODE_AUDIO_DELETE        601
#define CMD_CODE_AUDIO_LIST          602

/* Define Telemetry queue length */
#define TELEMETRY_QUEUE_LENGTH 10

typedef struct {
  /* data */
  bool isWifiConnected;
  bool isTimeSync;

  bool isTimeSyncCb;
  time_t epochtime;
} Sys_Info_Handle_t;

extern Sys_Info_Handle_t Sys_Info;

void User_System_Get_Config(void);

void User_System_Init(void);

bool Is_System_Time_Synchronized(void);

bool Is_System_Internet_Connected(void);
