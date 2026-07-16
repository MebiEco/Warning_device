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

#define DEVICE_ID_GROUP_1_NUM1 11
#define DEVICE_ID_GROUP_1_NUM2 12
#define DEVICE_ID_GROUP_1_NUM3 13
#define DEVICE_ID_GROUP_1_NUM4 14

#define DEVICE_ID_GROUP_2_NUM1 21
#define DEVICE_ID_GROUP_2_NUM2 22
#define DEVICE_ID_GROUP_2_NUM3 23
#define DEVICE_ID_GROUP_2_NUM4 24

#define DEVICE_ID_GROUP_3_NUM1 31
#define DEVICE_ID_GROUP_3_NUM2 32
#define DEVICE_ID_GROUP_3_NUM3 33
#define DEVICE_ID_GROUP_3_NUM4 34

#define DEVICE_ID_GROUP_4_NUM1 41
#define DEVICE_ID_GROUP_4_NUM2 42
#define DEVICE_ID_GROUP_4_NUM3 43
#define DEVICE_ID_GROUP_4_NUM4 44

/* Define for FRAM */
#define DEVICE_CONFIG_START_ADDRESS 0x1000
#define DEVICE_SPACE_LEN_IN_FRAM 512
#define NUM_OF_DEVICE 12

/* Define cmd code */
#define CMD_CODE_CONTROL_ON_OFF      101
#define CMD_CODE_CONTROL_SCHEDULE    102
#define CMD_CODE_UPDATE_FIRMWARE     501


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
