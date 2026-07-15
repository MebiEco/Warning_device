/*******************************************************************************
 * FILE: user_azure.c
 * MÔ TẢ: Kết nối và giao tiếp với Azure IoT Hub
 * 
 * CHỨC NĂNG CHÍNH:
 *   1. Kết nối MQTT tới Azure IoT Hub (TLS + Symmetric Key)
 *   2. Nhận lệnh từ cloud qua Direct Method (tên method: "Control")
 *   3. Gửi telemetry lên cloud
 * 
 * CÁC LỆNH DIRECT METHOD (gửi từ Azure Portal):
 *   - Method name: "Control"
 *   - Code 100: Phát file âm thanh từ SD card qua Bluetooth
 *     + Value=1: Phát (cần PondId, DeviceId, Duration, Repeat)
 *     + Value=0: Dừng phát
 *     + file_id = PondId * 100 + DeviceId → ví dụ: /sdcard/121.wav
 *   - Code 102: Điều khiển relay/chuông (GPIO 23)
 * 
 * CẤU HÌNH AZURE (có thể đổi từ trang web http://192.168.4.1):
 *   - Host Name, Device ID, Symmetric Key
 *   - Lưu trong NVS, tự nhớ sau khi restart
 ******************************************************************************/

#include "user_azure.h"
#include "user_system.h"
#include "azure_sample_connection.h"

/* Azure Provisioning/IoT Hub library includes */
#include "azure_iot_hub_client.h"
#include "azure_iot_hub_client_properties.h"
#include "azure_iot_provisioning_client.h"

/* Azure JSON includes */
#include "azure_iot_json_reader.h"
#include "azure_iot_json_writer.h"

/* Exponential backoff retry include. */
#include "backoff_algorithm.h"

/* Transport interface implementation include header for TLS. */
#include "transport_socket.h"
#include "transport_tls_socket.h"

/* Crypto helper header. */
#include "azure_sample_crypto.h"

/* Demo Specific configs. */
#include "demo_config.h"

/* Demo Specific Interface Functions. */
#include "azure_sample_connection.h"

/* Data Interface Definition */
// #include "sample_azure_iot_pnp_data_if.h"

#include "freertos/event_groups.h"
#include "user_http_server.h"
#include "user_ota.h"
#include "user_audio_files.h"
#include "esp_system.h"

#include "esp_log.h"

bool bIsOtaActivated = false;

#include "FreeRTOS.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "queue.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>

#undef AZLogInfo
#define AZLogInfo(...)

/* Declare task handle */
TaskHandle_t Azure_Process_Handle;
TaskHandle_t Azure_Transmit_Handle;

/* Declare IoT hub handle */
IoTHubHandle_t IoTHubHandle;

/* Declare mutex for receive and transmit proceess */
SemaphoreHandle_t azureMutex;

/* Declare queue for command and telemetry data */
QueueHandle_t xQueueTelemetry;
QueueHandle_t xQueueResponse;

/* Task Function Prototype */
static void Azure_Process_Loop_Task(void *pvParameters);
static void Azure_Transmit_Task(void *pvParameters);

/* Ensure NetworkContext is a complete type for stack allocation. The
 * middleware headers typedef an opaque struct; provide a minimal
 * definition used by the sample demos. */
struct NetworkContext {
  void *pParams;
};

/* Some demo symbols are provided by the sample application. Provide
 * minimal fallbacks here when they are not supplied by the project so
 * the file builds. These are guarded so a real sample implementation
 * will take precedence if present. */
#ifndef democonfigNETWORK_BUFFER_SIZE
#define democonfigNETWORK_BUFFER_SIZE 1024U
#endif

AzureIoTHubClient_t xAzureIoTHubClient;

static uint8_t ucMQTTMessageBuffer[democonfigNETWORK_BUFFER_SIZE];
static uint8_t ucScratchBuffer[512];
static uint8_t ucReportedPropertiesUpdate[380];
static uint32_t ulReportedPropertiesUpdateLength = 0U;

uint32_t ulScratchBufferLength = 0U;
NetworkCredentials_t xNetworkCredentials = {0};
AzureIoTTransportInterface_t xTransport;
NetworkContext_t xNetworkContext = {0};
TlsTransportParams_t xTlsTransportParams = {0};
AzureIoTResult_t xResult;
uint32_t ulStatus;
AzureIoTHubClientOptions_t xHubOptions = {0};
bool xSessionPresent;

uint8_t * pucIotHubHostname = (uint8_t *)IoTHubHandle.hostName;
uint8_t * pucIotHubDeviceId = (uint8_t *)IoTHubHandle.deviceId;


/* Simple unix time provider fallback. If your project provides a
 * higher-precision implementation, it will be used instead. */
uint64_t ullGetUnixTime(void) {
  time_t now = time(NULL);

  /* If system time is set (non-zero and reasonable), return epoch seconds.
   * Otherwise fall back to tick-based uptime (best-effort). */
  if (now > (time_t)1600000000) {
    return (uint64_t)now;
  }

  return (uint64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000ULL);
}

/* Minimal stubs for platform/sample helper functions. Real
 * implementations should be provided by the project (these return
 * success so demos can compile). */
static uint32_t
prvSetupNetworkCredentials(NetworkCredentials_t *pxNetworkCredentials) {
  pxNetworkCredentials->xDisableSni = pdFALSE;

  /* Set the credentials for establishing a TLS connection. */
  pxNetworkCredentials->pucRootCa =
      (const unsigned char *)democonfigROOT_CA_PEM;
  pxNetworkCredentials->xRootCaSize = sizeof(democonfigROOT_CA_PEM);

#ifdef democonfigCLIENT_CERTIFICATE_PEM
  pxNetworkCredentials->pucClientCert =
      (const unsigned char *)democonfigCLIENT_CERTIFICATE_PEM;
  pxNetworkCredentials->xClientCertSize =
      sizeof(democonfigCLIENT_CERTIFICATE_PEM);
  pxNetworkCredentials->pucPrivateKey =
      (const unsigned char *)democonfigCLIENT_PRIVATE_KEY_PEM;
  pxNetworkCredentials->xPrivateKeySize =
      sizeof(democonfigCLIENT_PRIVATE_KEY_PEM);
#endif

  return 0U;
}

static uint32_t
prvConnectToServerWithBackoffRetries(const char *pcHostName, uint32_t ulPort,
                                     NetworkCredentials_t *pxNetworkCredentials,
                                     NetworkContext_t *pxNetworkContext) {
  if ((pcHostName == NULL) || (pxNetworkCredentials == NULL) ||
      (pxNetworkContext == NULL)) {
    return 1U;
  }

  /* Use the TLS socket connect implementation to initialize transport
   * internals (this will allocate and set pxTlsParams->xSSLContext). */
  TlsTransportStatus_t xTlsStatus =
      TLS_Socket_Connect(pxNetworkContext, pcHostName, (uint16_t)ulPort,
                         pxNetworkCredentials, 2000U, 2000U);

  return (xTlsStatus == eTLSTransportSuccess) ? 0U : 1U;
}

static void prvHandleCloudMessageTest(
    AzureIoTHubClientCloudToDeviceMessageRequest_t *pxMessage,
    void *pvContext) {
  ESP_LOGI("AZURE:", "Receive C2D message: %.*s",
           (int)pxMessage->ulPayloadLength,
           (const char *)pxMessage->pvMessagePayload);
}

static void prvHandleCommand(AzureIoTHubClientCommandRequest_t *pxMessage,
                             void *pvContext) {
  uint16_t _code = 99;

  cJSON *res = cJSON_CreateObject();
  DirectMethodResponse_t response;
  memset(&response, 0, sizeof(response));

  ESP_LOGI("AZURE", "\n\n");
  ESP_LOGW("AZURE: ", "---------- Direct method ----------");
  ESP_LOGI("AZ: Direct method", "%.*s\nCommand name: %.*s",
           (int)pxMessage->ulPayloadLength, (char *)pxMessage->pvMessagePayload,
           (int)pxMessage->usCommandNameLength,
           (char *)pxMessage->pucCommandName);

  if (strncmp((const char *)pxMessage->pucCommandName, "Control", strlen("Control")) == 0) 
  {
    ESP_LOGI("AZURE: ", "Received direct method");

    char *buf = calloc(pxMessage->ulPayloadLength + 1, sizeof(char));
    if (buf != NULL) 
    {
      memcpy(buf, pxMessage->pvMessagePayload, pxMessage->ulPayloadLength);
      buf[pxMessage->ulPayloadLength] = '\0';

      cJSON *parsed = cJSON_Parse(buf);
      cJSON *code = cJSON_GetObjectItem(parsed, "Code");
      if (code) 
      {
        _code = code->valueint;
      }

      if (parsed) 
      {
        Azure_Handle_Direct_Method_Data(parsed, &response);
        cJSON_Delete(parsed);
      }else{
        ESP_LOGE("AZURE: ", "Parse json fail");
      }

      free(buf);

    }else{
      ESP_LOGE("AZURE: CMD CALLBACK", "Cannot allocate buffer for message");
      response.status = COMMAND_STATUS_DEVICE_ERROR;
      response.payloadLength =
          sprintf(response.payload, "Payload too long, device not enough ram");
    }

  }else{
    ESP_LOGE("AZURE: CMD CALLBACK", "Do not have Control method");
    response.status = COMMAND_STATUS_NOT_FOUND;
    response.payloadLength =
        sprintf(response.payload, "Method name: %.*s do not support",
                pxMessage->usCommandNameLength, pxMessage->pucCommandName);
  }

  cJSON_AddNumberToObject(res, "Code", _code);
  cJSON_AddNumberToObject(res, "TimeStamp", Sys_Info.epochtime);
  cJSON_AddStringToObject(res, "Message", response.payload);
  char *jsonStr = cJSON_PrintUnformatted(res);

  AzureIoTHubClient_SendCommandResponse(
      &xAzureIoTHubClient, pxMessage, response.status, (const uint8_t *)jsonStr,
      strlen(jsonStr));

  cJSON_Delete(res);
  free(jsonStr);

  ESP_LOGI(" ", "\n\n");
}

static void
prvHandleProperties(AzureIoTHubClientPropertiesResponse_t *pxMessage,
                    void *pvContext) {
  (void)pxMessage;
  (void)pvContext;
}

/* Provide the sample connection check used by the Azure demos. Forward
 * to the project's network status function so existing system code is
 * reused. */
bool xAzureSample_IsConnectedToInternet(void) 
{
  return Is_System_Internet_Connected();
}

/*******************************************************************************
 * HÀM: User_Azure_Connect
 * MÔ TẢ: Kết nối MQTT tới Azure IoT Hub
 *   - Sử dụng TLS + Symmetric Key để xác thực
 *   - Nếu thất bại → log lỗi và return (không crash)
 *   - Nếu thành công → subscribe Direct Method và C2D messages
 * 
 * KHI LỖI "not authorized":
 *   → Symmetric Key sai → vào http://192.168.4.1 để sửa
 ******************************************************************************/
void User_Azure_Connect(void) {
  uint32_t pulIothubHostnameLength = strlen(IoTHubHandle.hostName);
  uint32_t pulIothubDeviceIdLength = strlen(IoTHubHandle.deviceId);

  /* Initialize Azure IoT Middleware.  */
  if (AzureIoT_Init() != eAzureIoTSuccess) {
    ESP_LOGE("AZ IOT", "AzureIoT_Init failed");
    return;
  }

  ulStatus = prvSetupNetworkCredentials(&xNetworkCredentials);
  if (ulStatus != 0) {
    ESP_LOGE("AZ IOT", "Network credentials setup failed");
    return;
  }

  xNetworkContext.pParams = &xTlsTransportParams;

  if (xAzureSample_IsConnectedToInternet()) 
  {
    ulStatus = prvConnectToServerWithBackoffRetries(
        (const char *)pucIotHubHostname, democonfigIOTHUB_PORT,
        &xNetworkCredentials, &xNetworkContext);
    if (ulStatus != 0) {
      ESP_LOGE("AZ IOT", "TLS connection to %s failed", pucIotHubHostname);
      return;
    }

    /* Fill in Transport Interface send and receive function pointers. */
    xTransport.pxNetworkContext = &xNetworkContext;
    xTransport.xSend = TLS_Socket_Send;
    xTransport.xRecv = TLS_Socket_Recv;

    /* Init IoT Hub option */
    xResult = AzureIoTHubClient_OptionsInit(&xHubOptions);
    if (xResult != eAzureIoTSuccess) {
      ESP_LOGE("AZ IOT", "HubClient_OptionsInit failed");
      return;
    }

    xHubOptions.pucModuleID = (const uint8_t *)democonfigMODULE_ID;
    xHubOptions.ulModuleIDLength = sizeof(democonfigMODULE_ID) - 1;
    xHubOptions.pucModelID = (const uint8_t *)sampleazureiotMODEL_ID;
    xHubOptions.ulModelIDLength = sizeof(sampleazureiotMODEL_ID) - 1;

    xResult = AzureIoTHubClient_Init(&xAzureIoTHubClient, 
                                    pucIotHubHostname, pulIothubHostnameLength, 
                                    pucIotHubDeviceId, pulIothubDeviceIdLength, 
                                    &xHubOptions,
                                    ucMQTTMessageBuffer, 
                                    sizeof(ucMQTTMessageBuffer), 
                                    ullGetUnixTime,
                                    &xTransport);

    if (xResult != eAzureIoTSuccess) {
      ESP_LOGE("AZ IOT", "HubClient_Init failed");
      return;
    }

#ifdef democonfigDEVICE_SYMMETRIC_KEY
    xResult = AzureIoTHubClient_SetSymmetricKey(&xAzureIoTHubClient, 
                                                (const uint8_t *)IoTHubHandle.symmetricKey,
                                                strlen((const char *)IoTHubHandle.symmetricKey), 
                                                Crypto_HMAC);

    if (xResult != eAzureIoTSuccess) {
      ESP_LOGE("AZ IOT", "SetSymmetricKey failed");
      return;
    }
#endif /* democonfigDEVICE_SYMMETRIC_KEY */

    /* Sends an MQTT Connect packet over the already established TLS connection,
     * and waits for connection acknowledgment (CONNACK) packet. */
    LogInfo(("Creating an MQTT connection to %s.\r\n", pucIotHubHostname));

    xResult = AzureIoTHubClient_Connect(&xAzureIoTHubClient, 
                                        false,
                                        &xSessionPresent, 
                                        5000U);

    if (xResult != eAzureIoTSuccess) {
      ESP_LOGE("AZ IOT", "MQTT Connect failed! Check Azure credentials. Use http://192.168.4.1 to update.");
      return;
    }

    xResult = AzureIoTHubClient_SubscribeCloudToDeviceMessage(&xAzureIoTHubClient, 
                                                              prvHandleCloudMessageTest, 
                                                              &xAzureIoTHubClient,
                                                              5000U);
    if (xResult != eAzureIoTSuccess) {
      ESP_LOGE("AZ IOT", "Subscribe C2D failed");
      return;
    }

    xResult = AzureIoTHubClient_SubscribeCommand(&xAzureIoTHubClient, 
                                                  prvHandleCommand, 
                                                  &xAzureIoTHubClient, 
                                                  5000U);
    if (xResult != eAzureIoTSuccess) {
      ESP_LOGE("AZ IOT", "Subscribe Command failed");
      return;
    }

    IoTHubHandle.isAzureInitialized = true;

    if (IoTHubHandle.isProcessLoopInitialized == false) 
    {
      if (xTaskCreatePinnedToCore(Azure_Process_Loop_Task, "Azure process loop",
                                  2 * 4096, NULL, 5, &Azure_Process_Handle,
                                  0) == pdPASS) {
        ESP_LOGI("AZURE: PROCESS LOOP",
                 "Create process loop task suscessfully");
        // IoTHubHandle.isProcessLoopInitialized = true;
      } else {
        ESP_LOGI("AZURE: PROCESS LOOP", "Create process loop task fail\n");
      }
    }

  }
}


void User_Azure_Task(void) {
  /* Create mutex */
  azureMutex = xSemaphoreCreateMutex();

  if (azureMutex == NULL) 
  {
    ESP_LOGE("AZURE: CREATE MUTEX", "Cannot create mutex\n\n\n");
    while (1)
    {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

    while (1) 
  {
    if (bOtaPendingDownload) {
        bOtaPendingDownload = false;
        User_Ota_Prepare_System();
        ESP_LOGI("AZURE", "Starting OTA download from Azure task...");
        esp_err_t ota_ret = update_firmware(g_ota_update_url);
        if (ota_ret != ESP_OK) {
            ESP_LOGE("AZURE", "OTA failed: %s — rebooting", esp_err_to_name(ota_ret));
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    }

    audio_files_run_pending_download();

    if(!Is_System_Internet_Connected())
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if(strlen(IoTHubHandle.hostName) == 0 || strlen(IoTHubHandle.deviceId) == 0 || strlen(IoTHubHandle.symmetricKey) == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if(IoTHubHandle.isNeedReinit)
        {
            ESP_LOGW("AZURE: REINIT", "Releasing Azure client — reconnect with current config");
            if(xSemaphoreTake(azureMutex, pdMS_TO_TICKS(3000U)) == pdTRUE)
            {
                IoTHubHandle.isNeedReinit = false;
                if (IoTHubHandle.isAzureInitialized) {
                    AzureIoTHubClient_Deinit(&xAzureIoTHubClient);
                    TLS_Socket_Disconnect(&xNetworkContext);
                }
                IoTHubHandle.isAzureInitialized = false;
                xSemaphoreGive(azureMutex);
            }
            else
            {
                ESP_LOGE("AZURE: REINIT", "Cannot get mutex to deinit");
            }
        }

        if(bIsOtaActivated)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if(!IoTHubHandle.isAzureInitialized)
        {
            User_Azure_Connect();
        }

        while(1)
        {
            vTaskDelay(pdMS_TO_TICKS(500));
            if(IoTHubHandle.isNeedReinit || !Is_System_Internet_Connected() ||
               bOtaPendingDownload || bAudioDownloadPending)
            {
                break;
            }
        }
  }
}

static void Azure_Process_Loop_Task(void *pvParameters) {

  AzureIoTResult_t result;
  IoTHubHandle.isProcessLoopInitialized = true;

  while (1) {
    if (IoTHubHandle.isAzureInitialized && !bIsOtaActivated && !IoTHubHandle.isNeedReinit) {
      if (xSemaphoreTake(azureMutex, portMAX_DELAY) == pdTRUE) {
        result = AzureIoTHubClient_ProcessLoop(&xAzureIoTHubClient, 100U);
        if (result != eAzureIoTSuccess) {
          ESP_LOGE("AZURE: PROCESS LOOP", "Error code: %d", result);
          IoTHubHandle.isNeedReinit = true;
          IoTHubHandle.isAzureInitialized = false;
        }

        xSemaphoreGive(azureMutex);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static void Azure_Transmit_Task(void *pvParameters) {

  TelemetryEvent_t telemetry;
  AzureIoTResult_t result;

  xQueueTelemetry =
      xQueueCreate(TELEMETRY_QUEUE_LENGTH, sizeof(TelemetryEvent_t));
  if (xQueueTelemetry == NULL) {

    while (1) {
      ESP_LOGE("AZURE: TRANSMIT TASK",
               "Cannot create telemetry queue, need to check\n\n\n");
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
  } else {
    IoTHubHandle.isTransmitInitialized = true;
  }

  while (1)
  {
    // if (IoTHubHandle.isAzureInitialized)
    // {
    //   if (xQueueReceive(xQueueTelemetry, &telemetry, 0) == pdTRUE) {
    //     if (xSemaphoreTake(azureMutex, pdMS_TO_TICKS(3000U)) == pdTRUE) {
    //       result = AzureIoTHubClient_SendTelemetry(
    //           &xAzureIoTHubClient, (const uint8_t *)telemetry.payload,
    //           strlen(telemetry.payload), NULL, eAzureIoTHubMessageQoS1, NULL);
    //       xSemaphoreGive(azureMutex);
    //     } else {
    //       ESP_LOGE("AZURE: TRANSMIT TASK", "Cannot get mutex");
    //     }
    //   }
    // }
    if (xQueueReceive(xQueueTelemetry, &telemetry, 0) == pdTRUE)
    {
      if(!Is_System_Internet_Connected() || !IoTHubHandle.isAzureInitialized || IoTHubHandle.isNeedReinit)
      {
        ESP_LOGW("AZURE: TRANSMIT TASK", "Skip telemetry (not connected)");
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }

      if (xSemaphoreTake(azureMutex, pdMS_TO_TICKS(3000U)) == pdTRUE)
      {
        result = AzureIoTHubClient_SendTelemetry(&xAzureIoTHubClient, (const uint8_t *)telemetry.payload, strlen(telemetry.payload), NULL, eAzureIoTHubMessageQoS1, NULL);
        if(result != eAzureIoTSuccess)
        {
          ESP_LOGE("AZURE: TRANSMIT TASK", "Send telemetry failed: %d", result);
          IoTHubHandle.isNeedReinit = true;
          IoTHubHandle.isAzureInitialized = false;
        }
        xSemaphoreGive(azureMutex);
      }
      else
      {
        ESP_LOGE("AZURE: TRANSMIT TASK", "Cannot get mutex");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}


/* Push Telemetry to queue */
BaseType_t PushTelemetry(const char *payload) {
  if (IoTHubHandle.isTransmitInitialized) {
    TelemetryEvent_t event;
    memset(&event, 0, sizeof(event));
    strncpy(event.payload, payload, sizeof(event.payload) - 1);

    if (xQueueSend(xQueueTelemetry, &event, 0) == pdPASS) {
      return pdPASS;
    } else {
      ESP_LOGW("AZURE:", "Telemetry queue full, drop message");
      return pdFAIL;
    }
  } else {
    ESP_LOGW("AZURE: PUSH TELEMETRY TO QUEU", "Telemetry do not inited yet");
    return pdFAIL;
  }
}

/* Handle direct method */
/*******************************************************************************
 * HÀM: Azure_Handle_Direct_Method_Data
 * MÔ TẢ: Xử lý lệnh Direct Method từ Azure IoT Hub
 * 
 * CÁC MÃ LỆNH:
 *   Code 100 - PHÁT ÂM THANH:
 *     Payload: {"Code":100,"Data":{"Value":1,"PondId":1,"DeviceId":21,"Duration":10,"Repeat":3}}
 *     file_id = PondId * 100 + DeviceId = 121 → phát /sdcard/121.wav
 *     Value=0 → dừng phát ngay lập tức
 * 
 *   Code 102 - ĐIỀU KHIỂN RELAY (GPIO 23):
 *     Payload: {"Code":102,"Data":{"Value":1}}
 *     Value=1 → bật relay, Value=0 → tắt relay
 ******************************************************************************/
void Azure_Handle_Direct_Method_Data(cJSON *payload, DirectMethodResponse_t *response) 
{
    uint16_t _code =0;

    cJSON *code_item = cJSON_GetObjectItem(payload, "Code");
    cJSON *data_item = cJSON_GetObjectItem(payload, "Data");

    if ((code_item != NULL) && (data_item != NULL))
    {
        _code = code_item->valueint;
        if(_code == CMD_CODE_LOA) //code=100
        {
          cJSON *Value = cJSON_GetObjectItem(data_item, "Value");
          cJSON *PondId = cJSON_GetObjectItem(data_item, "PondId");
          cJSON *DeviceId = cJSON_GetObjectItem(data_item, "DeviceId");
          cJSON *Duration = cJSON_GetObjectItem(data_item, "Duration");
          cJSON *Repeat = cJSON_GetObjectItem(data_item, "Repeat");
          cJSON *FileName = cJSON_GetObjectItem(data_item, "FileName");

          if (Value == NULL || !cJSON_IsNumber(Value))
          {
            response->status = COMMAND_STATUS_BAD_REQUEST;
            response->payloadLength = sprintf(response->payload, "{\"error\":\"Missing Value\"}");
            return;
          }

          ESP_LOGI("AZURE: ", "---------- CMD CALLBACK ----------");
          ESP_LOGI("AZURE: ", "Code: %d", _code);
          if (Value->valueint == 0)
          {
              Stop_Alarm();
              response->status = COMMAND_STATUS_OK;
              response->payloadLength = sprintf(response->payload, "{\"status\": \"alarm stopped\"}");
              ESP_LOGW("AZURE", "Alarm stop command received. Stopping immediately.");
              return;
          }

          if (!Duration || !cJSON_IsNumber(Duration) || !Repeat || !cJSON_IsNumber(Repeat))
          {
              response->status = COMMAND_STATUS_BAD_REQUEST;
              response->payloadLength = sprintf(response->payload,
                  "{\"error\": \"Missing Duration or Repeat\"}");
              return;
          }

          AlarmCmd_t cmd;
          memset(&cmd, 0, sizeof(cmd));
          cmd.duration_sec = Duration->valueint;
          cmd.repeat_count = Repeat->valueint;

          if (FileName && cJSON_IsString(FileName) && FileName->valuestring && FileName->valuestring[0])
          {
              if (!audio_files_sanitize_name(FileName->valuestring, cmd.filename, sizeof(cmd.filename)))
              {
                  response->status = COMMAND_STATUS_BAD_REQUEST;
                  response->payloadLength = sprintf(response->payload, "{\"error\":\"Invalid FileName\"}");
                  return;
              }
          }
          else
          {
              if (!PondId || !cJSON_IsNumber(PondId) || !DeviceId || !cJSON_IsNumber(DeviceId))
              {
                  response->status = COMMAND_STATUS_BAD_REQUEST;
                  response->payloadLength = sprintf(response->payload,
                      "{\"error\": \"Need FileName or PondId+DeviceId\"}");
                  return;
              }
              cmd.file_id = PondId->valueint * 100 + DeviceId->valueint;
          }

          if (alarm_queue != NULL)
          {
            if (xQueueSend(alarm_queue, &cmd, 0) == pdTRUE)
            {
              response->status = COMMAND_STATUS_OK;
              if (cmd.filename[0]) {
                response->payloadLength = sprintf(response->payload,
                        "{\"status\": \"queued\", \"file\": \"/sdcard/%s\"}", cmd.filename);
              } else {
                response->payloadLength = sprintf(response->payload,
                        "{\"status\": \"queued\", \"file\": \"/sdcard/%d.wav\"}", cmd.file_id);
              }
              ESP_LOGI("AZURE", "Queued alarm file=%s id=%d",
                       cmd.filename[0] ? cmd.filename : "(id)", cmd.file_id);
            }
            else
            {
              response->status = COMMAND_STATUS_BAD_REQUEST;
              sprintf(response->payload, "Bad request");
              ESP_LOGE("AZURE", "Alarm queue full!");
            }
          }
        }
        else if(_code == CMD_CODE_CONTROL_ON_OFF) //code=101
        {
          cJSON *Value = cJSON_GetObjectItem(data_item, "Value");
          cJSON *Duration = cJSON_GetObjectItem(data_item, "Duration");
          cJSON *RestTime = cJSON_GetObjectItem(data_item, "RestTime");
          cJSON *Repeat = cJSON_GetObjectItem(data_item, "Repeat");

          if((Value != NULL) && (Duration != NULL) && (RestTime != NULL) && (Repeat != NULL))
          {
            ESP_LOGI("AZURE: ", "---------- CMD CALLBACK ----------");
            ESP_LOGI("AZURE: ", "Code: %d", _code);
            if(Value->valueint == 0)
            {
              SirenCmd_t stop_cmd = { .value = 0 };
              if (siren_queue != NULL)
              {
                  xQueueReset(siren_queue);                          // xóa hết lệnh chờ
                  xQueueSend(siren_queue, &stop_cmd, 0);             // gửi lệnh dừng
              }
              response->status = COMMAND_STATUS_OK;
              response->payloadLength = sprintf(response->payload, "{\"status\": \"bell stopped\"}");
              ESP_LOGW("AZURE", "Bell stop command received.");
              return;
            }
            if (!Duration || !cJSON_IsNumber(Duration))
            {
              response->status = COMMAND_STATUS_BAD_REQUEST;
              response->payloadLength = sprintf(response->payload,
                    "{\"error\": \"Missing or invalid field: Duration (seconds)\"}");
                ESP_LOGE("AZURE", "Bell cmd rejected: missing Duration");
                return;
            }
            if(!RestTime || !cJSON_IsNumber(RestTime))
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = sprintf(response->payload,
                    "{\"error\": \"Missing or invalid field: RestTime (seconds)\"}");
                ESP_LOGE("AZURE", "Bell cmd rejected: missing RestTime");
                return;
            }
            if (!Repeat || !cJSON_IsNumber(Repeat))
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = sprintf(response->payload,
                    "{\"error\": \"Missing or invalid field: Repeat\"}");
                ESP_LOGE("AZURE", "Bell cmd rejected: missing Repeat");
                return;
            }

            SirenCmd_t cmd;
            cmd.value        = 1;
            cmd.duration_sec = Duration->valueint;
            cmd.rest_sec     = RestTime->valueint;
            cmd.repeat       = Repeat->valueint;

            if (siren_queue != NULL)
            {
                if (xQueueSend(siren_queue, &cmd, 0) == pdTRUE)
                {
                    response->status = COMMAND_STATUS_OK;
                    response->payloadLength = sprintf(response->payload,
                        "{\"status\": \"bell started\", \"duration_s\": %d, \"rest_s\": %d, \"repeat\": %d}",
                        cmd.duration_sec, cmd.rest_sec, cmd.repeat);
                    ESP_LOGI("AZURE", "Bell start: dur=%ds rest=%ds repeat=%d",
                            cmd.duration_sec, cmd.rest_sec, cmd.repeat);
                }
                else
                {
                    // response->status = COMMAND_STATUS_DEVICE_ERROR;
                    // response->payloadLength = sprintf(response->payload, "{\"error\": \"Siren queue full\"}");
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    sprintf(response->payload, "Bad request");
                    ESP_LOGE("AZURE", "Siren queue full!");
                }
            }
        // else
        // {
        //     response->status = COMMAND_STATUS_DEVICE_ERROR;
        //     response->payloadLength = sprintf(response->payload, "{\"error\": \"Siren queue not initialized\"}");
        //     ESP_LOGE("AZURE", "Siren queue not initialized!");
        // }
        // return;
          }
        }

        else if (_code == CMD_CODE_AUDIO_DOWNLOAD) // 600
        {
            cJSON *Url = cJSON_GetObjectItem(data_item, "Url");
            cJSON *FileName = cJSON_GetObjectItem(data_item, "FileName");
            if (!Url || !cJSON_IsString(Url) || !FileName || !cJSON_IsString(FileName))
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = sprintf(response->payload, "{\"error\":\"Need Url and FileName\"}");
                return;
            }
            if (!audio_files_sd_ready())
            {
                response->status = COMMAND_STATUS_DEVICE_ERROR;
                response->payloadLength = sprintf(response->payload, "{\"error\":\"SD card not ready\"}");
                return;
            }
            if (!audio_files_request_download(Url->valuestring, FileName->valuestring))
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = sprintf(response->payload, "{\"error\":\"Invalid Url/FileName or busy\"}");
                return;
            }
            response->status = COMMAND_STATUS_OK;
            response->payloadLength = sprintf(response->payload, "{\"status\":\"download queued\"}");
        }
        else if (_code == CMD_CODE_AUDIO_DELETE) // 601
        {
            cJSON *FileName = cJSON_GetObjectItem(data_item, "FileName");
            if (!FileName || !cJSON_IsString(FileName))
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = sprintf(response->payload, "{\"error\":\"Need FileName\"}");
                return;
            }
            esp_err_t e = audio_files_delete(FileName->valuestring);
            if (e == ESP_ERR_INVALID_STATE)
            {
                response->status = COMMAND_STATUS_DEVICE_ERROR;
                response->payloadLength = sprintf(response->payload, "{\"error\":\"SD card not ready\"}");
            }
            else if (e != ESP_OK)
            {
                response->status = COMMAND_STATUS_DEVICE_ERROR;
                response->payloadLength = sprintf(response->payload, "{\"error\":\"delete failed\"}");
            }
            else
            {
                response->status = COMMAND_STATUS_OK;
                response->payloadLength = sprintf(response->payload, "{\"status\":\"deleted\"}");
            }
        }
        else if (_code == CMD_CODE_AUDIO_LIST) // 602
        {
            if (!audio_files_sd_ready())
            {
                response->status = COMMAND_STATUS_DEVICE_ERROR;
                response->payloadLength = sprintf(response->payload, "{\"error\":\"SD card not ready\"}");
                return;
            }
            char *list = audio_files_list_json();
            if (!list)
            {
                response->status = COMMAND_STATUS_DEVICE_ERROR;
                response->payloadLength = sprintf(response->payload, "{\"error\":\"list failed\"}");
                return;
            }
            response->status = COMMAND_STATUS_OK;
            /* Direct method response buffer is 256 — truncate safely */
            response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                                               "{\"files\":%s}", list);
            if (response->payloadLength >= (int)sizeof(response->payload)) {
                response->payloadLength = sizeof(response->payload) - 1;
            }
            free(list);
        }

        else if(_code == CMD_CODE_UPDATE_FIRMWARE) //code = 501
        {
            cJSON *Version = cJSON_GetObjectItem(data_item, "Version");
            cJSON *Url = cJSON_GetObjectItem(data_item, "Url");

            if((Version != NULL) && cJSON_IsString(Version) && (Url != NULL) && cJSON_IsString(Url))
            {
                ESP_LOGI("AZURE: ", "---------- UPDATE FIRMWARE ----------");
                ESP_LOGI("AZURE: UPDATE FIRMWARE", "Version: %s, Url: %s\n", Version->valuestring, Url->valuestring);

                response->status = COMMAND_STATUS_OK;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload), "Update version %s successfull", Version->valuestring);
                ESP_LOGI("AZURE: UPDATE FIRMWARE", "Preparing to update version %s", Version->valuestring);

                // Execute Telemetry First to Unblock Backend Thread Cleanly
                cJSON *tele = cJSON_CreateObject();
                cJSON *payload = cJSON_CreateObject();
                if(tele != NULL && payload != NULL)
                {
                    char *tele_str;
                    cJSON_AddNumberToObject(tele, "status", 200);
                    cJSON_AddItemToObject(tele, "payload", payload);
                    cJSON_AddNumberToObject(payload, "Code", 501);
                    cJSON_AddNumberToObject(payload, "TimeStamp", (double)Sys_Info.epochtime);
                    cJSON_AddStringToObject(payload, "Message", response->payload);

                    tele_str = cJSON_PrintUnformatted(tele);
                    if(tele_str != NULL)
                    {
                        PushTelemetry(tele_str);
                        free(tele_str);
                    }
                    else
                    {
                        ESP_LOGE("AZURE: CMD CALLBACK", "Telemetry json build failed");
                    }
                    ESP_LOGI("AZURE: UPDATE FIRMWARE", "Firmware dispatching to existing user_ota.c Daemon task...");
                }
                else
                {
                    ESP_LOGE("AZURE: CMD CALLBACK", "Telemetry alloc failed");
                }
                
                if(tele != NULL)
                {
                    cJSON_Delete(tele);
                }
                else if(payload != NULL)
                {
                    cJSON_Delete(payload);
                }

                if (!User_Ota_Request_Update(Url->valuestring)) {
                    response->status = COMMAND_STATUS_DEVICE_ERROR;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                        "OTA start failed (low memory)");
                    ESP_LOGE("AZURE: UPDATE FIRMWARE", "OTA request failed");
                }

            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = sprintf(response->payload, "Missing Version/Url");
                ESP_LOGE("AZURE: UPDATE FIRMWARE", "Missing Version or Url field");
            }

    }
    else
    {
        response->status = COMMAND_STATUS_BAD_REQUEST;
        response->payloadLength = sprintf(response->payload, "{\"error\": \"Missing Data object\"}");
        return;
    }
  }
}

    // ----------------------------------------------------------------
    // Code 100 - Điều khiển loa (phát file âm thanh từ SD card)
    // ----------------------------------------------------------------
    // if (code == 100)
    // {
    //     cJSON *value_item     = cJSON_GetObjectItem(data_obj, "Value");
    //     int value = (value_item && cJSON_IsNumber(value_item)) ? value_item->valueint : 1;

    //     // Value = 0: dừng loa ngay lập tức, xóa hết lệnh chờ
    //     if (value == 0)
    //     {
    //         Stop_Alarm();
    //         response->status = COMMAND_STATUS_OK;
    //         response->payloadLength = sprintf(response->payload, "{\"status\": \"alarm stopped\"}");
    //         ESP_LOGW("AZURE", "Alarm stop command received. Stopping immediately.");
    //         return;
    //     }

    //     // Value = 1: xếp lệnh phát vào queue
    //     cJSON *pond_id_item   = cJSON_GetObjectItem(data_obj, "PondId");
    //     cJSON *device_id_item = cJSON_GetObjectItem(data_obj, "DeviceId");
    //     cJSON *duration_item  = cJSON_GetObjectItem(data_obj, "Duration");
    //     cJSON *repeat_item    = cJSON_GetObjectItem(data_obj, "Repeat");

    //     // Kiểm tra đủ các trường bắt buộc
    //     if (!pond_id_item || !cJSON_IsNumber(pond_id_item))
    //     {
    //         response->status = COMMAND_STATUS_BAD_REQUEST;
    //         response->payloadLength = sprintf(response->payload,
    //             "{\"error\": \"Missing or invalid field: PondId\"}");
    //         ESP_LOGE("AZURE", "Alarm cmd rejected: missing PondId");
    //         return;
    //     }
    //     if (!device_id_item || !cJSON_IsNumber(device_id_item))
    //     {
    //         response->status = COMMAND_STATUS_BAD_REQUEST;
    //         response->payloadLength = sprintf(response->payload,
    //             "{\"error\": \"Missing or invalid field: DeviceId\"}");
    //         ESP_LOGE("AZURE", "Alarm cmd rejected: missing DeviceId");
    //         return;
    //     }
    //     if (!duration_item || !cJSON_IsNumber(duration_item))
    //     {
    //         response->status = COMMAND_STATUS_BAD_REQUEST;
    //         response->payloadLength = sprintf(response->payload,
    //             "{\"error\": \"Missing or invalid field: Duration (seconds)\"}");
    //         ESP_LOGE("AZURE", "Alarm cmd rejected: missing Duration");
    //         return;
    //     }
    //     if (!repeat_item || !cJSON_IsNumber(repeat_item))
    //     {
    //         response->status = COMMAND_STATUS_BAD_REQUEST;
    //         response->payloadLength = sprintf(response->payload,
    //             "{\"error\": \"Missing or invalid field: Repeat\"}");
    //         ESP_LOGE("AZURE", "Alarm cmd rejected: missing Repeat");
    //         return;
    //     }

    //     int pond_id   = pond_id_item->valueint;
    //     int device_id = device_id_item->valueint;

    //     AlarmCmd_t cmd;
    //     cmd.file_id      = pond_id * 100 + device_id;
    //     cmd.duration_sec = duration_item->valueint;
    //     cmd.repeat_count = repeat_item->valueint;

    //     if (alarm_queue != NULL)
    //     {
    //         if (xQueueSend(alarm_queue, &cmd, 0) == pdTRUE)
    //         {
    //             response->status = COMMAND_STATUS_OK;
    //             response->payloadLength = sprintf(response->payload,
    //                 "{\"status\": \"queued\", \"file\": \"/sdcard/%d.wav\"}", cmd.file_id);
    //             ESP_LOGI("AZURE", "Queued alarm: %d (Pond:%d Dev:%d) Repeat:%d Dur:%ds",
    //                      cmd.file_id, pond_id, device_id, cmd.repeat_count, cmd.duration_sec);
    //         }
    //         else
    //         {
    //             response->status = COMMAND_STATUS_DEVICE_ERROR;
    //             response->payloadLength = sprintf(response->payload, "{\"error\": \"Alarm queue full\"}");
    //         }
    //     }
    //     else
    //     {
    //         response->status = COMMAND_STATUS_DEVICE_ERROR;
    //         response->payloadLength = sprintf(response->payload, "{\"error\": \"Alarm queue not initialized\"}");
    //     }
    //     return;
    // }



    // ----------------------------------------------------------------
    // Code 102 - Điều khiển chuông (GPIO relay)
    // ----------------------------------------------------------------
//     if (code == 102)
//     {
//         cJSON *value_item = cJSON_GetObjectItem(data_obj, "Value");
//         int value = (value_item && cJSON_IsNumber(value_item)) ? value_item->valueint : 1;

//         // Value = 0: dừng chuông ngay, không cần xét field khác
//         if (value == 0)
//         {
//             SirenCmd_t stop_cmd = { .value = 0 };
//             if (siren_queue != NULL)
//             {
//                 xQueueReset(siren_queue);                          // xóa hết lệnh chờ
//                 xQueueSend(siren_queue, &stop_cmd, 0);             // gửi lệnh dừng
//             }
//             response->status = COMMAND_STATUS_OK;
//             response->payloadLength = sprintf(response->payload, "{\"status\": \"bell stopped\"}");
//             ESP_LOGW("AZURE", "Bell stop command received.");
//             return;
//         }

//         // Value = 1: bật chuông theo thông số
//         cJSON *duration_item = cJSON_GetObjectItem(data_obj, "Duration");
//         cJSON *rest_item     = cJSON_GetObjectItem(data_obj, "RestTime");
//         cJSON *repeat_item   = cJSON_GetObjectItem(data_obj, "Repeat");

//         // Kiểm tra đủ các trường bắt buộc
//         if (!duration_item || !cJSON_IsNumber(duration_item))
//         {
//             response->status = COMMAND_STATUS_BAD_REQUEST;
//             response->payloadLength = sprintf(response->payload,
//                 "{\"error\": \"Missing or invalid field: Duration (seconds)\"}");
//             ESP_LOGE("AZURE", "Bell cmd rejected: missing Duration");
//             return;
//         }
//         if (!rest_item || !cJSON_IsNumber(rest_item))
//         {
//             response->status = COMMAND_STATUS_BAD_REQUEST;
//             response->payloadLength = sprintf(response->payload,
//                 "{\"error\": \"Missing or invalid field: RestTime (seconds)\"}");
//             ESP_LOGE("AZURE", "Bell cmd rejected: missing RestTime");
//             return;
//         }
//         if (!repeat_item || !cJSON_IsNumber(repeat_item))
//         {
//             response->status = COMMAND_STATUS_BAD_REQUEST;
//             response->payloadLength = sprintf(response->payload,
//                 "{\"error\": \"Missing or invalid field: Repeat\"}");
//             ESP_LOGE("AZURE", "Bell cmd rejected: missing Repeat");
//             return;
//         }

//         SirenCmd_t cmd;
//         cmd.value        = 1;
//         cmd.duration_sec = duration_item->valueint;
//         cmd.rest_sec     = rest_item->valueint;
//         cmd.repeat       = repeat_item->valueint;

//         if (siren_queue != NULL)
//         {
//             if (xQueueSend(siren_queue, &cmd, 0) == pdTRUE)
//             {
//                 response->status = COMMAND_STATUS_OK;
//                 response->payloadLength = sprintf(response->payload,
//                     "{\"status\": \"bell started\", \"duration_s\": %d, \"rest_s\": %d, \"repeat\": %d}",
//                     cmd.duration_sec, cmd.rest_sec, cmd.repeat);
//                 ESP_LOGI("AZURE", "Bell start: dur=%ds rest=%ds repeat=%d",
//                          cmd.duration_sec, cmd.rest_sec, cmd.repeat);
//             }
//             else
//             {
//                 response->status = COMMAND_STATUS_DEVICE_ERROR;
//                 response->payloadLength = sprintf(response->payload, "{\"error\": \"Siren queue full\"}");
//                 ESP_LOGE("AZURE", "Siren queue full!");
//             }
//         }
//         else
//         {
//             response->status = COMMAND_STATUS_DEVICE_ERROR;
//             response->payloadLength = sprintf(response->payload, "{\"error\": \"Siren queue not initialized\"}");
//             ESP_LOGE("AZURE", "Siren queue not initialized!");
//         }
//         return;
//     }




//     // Fallback - code không nhận dạng được
//     response->status = COMMAND_STATUS_BAD_REQUEST;
//     response->payloadLength = sprintf(response->payload, "{\"error\": \"Unknown Code: %d\"}", code);
//     ESP_LOGE("AZURE", "Unknown direct method code: %d", code);
// }

