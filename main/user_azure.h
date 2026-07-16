#pragma once

void User_Azure_Task(void);

#include "azure_iot_hub_client.h"
#include "cJSON.h"
#include "stdbool.h"
#include "stdint.h"

#define IOT_HUB_HOST_NAME_LEN 64
#define IOT_HUB_DEVICE_ID_LEN 32
#define IOT_HUB_SYMMETRIC_KEY_LEN 64

/* Command codes: see user_system.h (CMD_CODE_*) */

typedef struct {
  char hostName[IOT_HUB_HOST_NAME_LEN];
  char deviceId[IOT_HUB_DEVICE_ID_LEN];
  char symmetricKey[IOT_HUB_SYMMETRIC_KEY_LEN];

  bool isNeedReinit;
  bool isAzureInitialized;
  bool isProcessLoopInitialized;
  bool isTransmitInitialized;

} IoTHubHandle_t;

extern IoTHubHandle_t IoTHubHandle;
extern bool bIsOtaActivated;
extern TaskHandle_t Azure_Process_Handle;

typedef struct {
  char payload[1024];
} TelemetryEvent_t;

typedef enum {
  COMMAND_STATUS_OK = 200,
  COMMAND_STATUS_BAD_REQUEST = 400,
  COMMAND_STATUS_NOT_FOUND = 404,
  COMMAND_STATUS_TOO_MANY_REQUEST = 429,
  COMMAND_STATUS_DEVICE_ERROR = 500
} CommandStatus_t;

typedef struct {
  CommandStatus_t status;
  char payload[1536];
  uint16_t payloadLength;
  /** If true, payload is the full DM response JSON (e.g. Code 300 snapshot). */
  bool payload_is_full_body;
} DirectMethodResponse_t;

void User_Azure_Task(void);

BaseType_t PushTelemetry(const char *payload);

void Azure_Handle_Direct_Method_Data(cJSON *payload,
                                     DirectMethodResponse_t *response);
