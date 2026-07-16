#include "user_bluetooth.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bt_app_core.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"

#include "user_excute_audio.h" // For bt_app_a2d_data_cb references (is_playing, psram_audio_buffer, etc.)
#include "user_system.h"     // For User_System_Get_Bluetooth_Config references (get_bt_mac, get_bt_name, etc.)

/* log tags */
#define BT_AV_TAG "BT_AV"
#define BT_RC_CT_TAG "RC_CT"

#define LOCAL_DEVICE_NAME "ESP_WARNING_DEVICE"

#define APP_RC_CT_TL_GET_CAPS (0)
#define APP_RC_CT_TL_RN_VOLUME_CHANGE (1)

enum {
  BT_APP_STACK_UP_EVT = 0x0000,   
  BT_APP_HEART_BEAT_EVT = 0xff00, 
};

enum {
  APP_AV_STATE_IDLE,
  APP_AV_STATE_DISCOVERING,
  APP_AV_STATE_DISCOVERED,
  APP_AV_STATE_UNCONNECTED,
  APP_AV_STATE_CONNECTING,
  APP_AV_STATE_CONNECTED,
  APP_AV_STATE_DISCONNECTING,
};

enum {
  APP_AV_MEDIA_STATE_IDLE,
  APP_AV_MEDIA_STATE_STARTING,
  APP_AV_MEDIA_STATE_STARTED,
  APP_AV_MEDIA_STATE_STOPPING,
};

static void bt_av_hdl_stack_evt(uint16_t event, void *p_param);
static void bt_av_hdl_avrc_ct_evt(uint16_t event, void *p_param);
static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
static int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len);
static void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
static void bt_app_a2d_heart_beat(TimerHandle_t arg);
static void bt_app_av_sm_hdlr(uint16_t event, void *param);
static char *bda2str(esp_bd_addr_t bda, char *str, size_t size);
static void bt_app_av_state_unconnected_hdlr(uint16_t event, void *param);
static void bt_app_av_state_connecting_hdlr(uint16_t event, void *param);
static void bt_app_av_state_connected_hdlr(uint16_t event, void *param);
static void bt_app_av_state_disconnecting_hdlr(uint16_t event, void *param);

static esp_bd_addr_t s_peer_bda = {0}; 
static uint8_t s_peer_bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];            
static int s_a2d_state = APP_AV_STATE_IDLE; 
static int s_media_state = APP_AV_MEDIA_STATE_IDLE; 
static int s_intv_cnt = 0;   
static int s_connecting_intv = 0;                         
static uint32_t s_pkt_cnt = 0; 
static esp_avrc_rn_evt_cap_mask_t s_avrc_peer_rn_cap;     
static TimerHandle_t s_tmr; 

bt_device_info_t discovered_devices[MAX_DISCOVERED_DEVICES];
int discovered_devices_count = 0;
bool is_manual_connect_requested = false;
int s_bt_retry_count = 0;
static bool is_bt_initialized = false;

#define NVS_BT_NAMESPACE "bt_config"
#define NVS_BT_MAC_KEY "bt_mac"
#define NVS_BT_NAME_KEY "bt_name"

void save_bt_device_to_nvs(esp_bd_addr_t bda, const char *name)
{
  nvs_handle_t my_handle;
  esp_err_t err = nvs_open(NVS_BT_NAMESPACE, NVS_READWRITE, &my_handle);
  if (err != ESP_OK)
    return;

  nvs_set_blob(my_handle, NVS_BT_MAC_KEY, bda, ESP_BD_ADDR_LEN);
  nvs_set_str(my_handle, NVS_BT_NAME_KEY, name);
  nvs_commit(my_handle);
  nvs_close(my_handle);
  ESP_LOGI(BT_AV_TAG, "Saved BT device to NVS: %s", name);
}

bool load_bt_device_from_nvs(esp_bd_addr_t bda, char *name)
{
  nvs_handle_t my_handle;
  esp_err_t err = nvs_open(NVS_BT_NAMESPACE, NVS_READONLY, &my_handle);
  if (err != ESP_OK)
    return false;

  size_t required_size = ESP_BD_ADDR_LEN;
  err = nvs_get_blob(my_handle, NVS_BT_MAC_KEY, bda, &required_size);
  if (err != ESP_OK)
  {
    nvs_close(my_handle);
    return false;
  }

  required_size = ESP_BT_GAP_MAX_BDNAME_LEN + 1;
  err = nvs_get_str(my_handle, NVS_BT_NAME_KEY, name, &required_size);
  nvs_close(my_handle);
  return err == ESP_OK;
}

void start_bt_device_scan(void)
{
  discovered_devices_count = 0;
  memset(discovered_devices, 0, sizeof(discovered_devices));
  is_manual_connect_requested = false;   
  s_bt_retry_count = 999;                
  esp_a2d_source_disconnect(s_peer_bda); 

  ESP_LOGI(BT_AV_TAG, "Starting device discovery manually...");
  s_a2d_state = APP_AV_STATE_DISCOVERING;
  esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
}

void connect_to_bt_device(uint8_t *mac_addr, const char *name)
{
  memcpy(s_peer_bda, mac_addr, ESP_BD_ADDR_LEN);
  if (name != NULL)
  {
    strncpy((char *)s_peer_bdname, name, ESP_BT_GAP_MAX_BDNAME_LEN);
    save_bt_device_to_nvs(s_peer_bda, name);
  }
  ESP_LOGI(BT_AV_TAG, "Device selected and saved. Canceling discovery...");
  
  /* We just cancel discovery. We will NOT change state to APP_AV_STATE_DISCOVERED 
     because we don't want it to actually connect in Config Mode. */
  esp_bt_gap_cancel_discovery();
}

static char *bda2str(esp_bd_addr_t bda, char *str, size_t size)
{
  if (bda == NULL || str == NULL || size < 18) { return NULL; }
  sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x", bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
  return str;
}

static bool get_name_from_eir(uint8_t *eir, uint8_t *bdname, uint8_t *bdname_len)
{
  uint8_t *rmt_bdname = NULL;
  uint8_t rmt_bdname_len = 0;

  if (!eir) return false;

  rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
  if (!rmt_bdname) {
    rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
  }

  if (rmt_bdname) {
    if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
      rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
    }
    if (bdname) {
      memcpy(bdname, rmt_bdname, rmt_bdname_len);
      bdname[rmt_bdname_len] = '\0';
    }
    if (bdname_len) *bdname_len = rmt_bdname_len;
    return true;
  }
  return false;
}

static void filter_inquiry_scan_result(esp_bt_gap_cb_param_t *param)
{
  char bda_str[18];
  uint32_t cod = 0;    
  int32_t rssi = -129; 
  uint8_t *eir = NULL;
  esp_bt_gap_dev_prop_t *p;

  ESP_LOGI(BT_AV_TAG, "Scanned device: %s", bda2str(param->disc_res.bda, bda_str, 18));
  for (int i = 0; i < param->disc_res.num_prop; i++)
  {
    p = param->disc_res.prop + i;
    switch (p->type)
    {
    case ESP_BT_GAP_DEV_PROP_COD:
      cod = *(uint32_t *)(p->val);
      ESP_LOGI(BT_AV_TAG, "--Class of Device: 0x%" PRIx32, cod);
      break;
    case ESP_BT_GAP_DEV_PROP_RSSI:
      rssi = *(int8_t *)(p->val);
      ESP_LOGI(BT_AV_TAG, "--RSSI: %" PRId32, rssi);
      break;
    case ESP_BT_GAP_DEV_PROP_EIR:
      eir = (uint8_t *)(p->val);
      break;
    case ESP_BT_GAP_DEV_PROP_BDNAME:
    default:
      break;
    }
  }

  char device_name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = {0};
  if (eir) {
    get_name_from_eir(eir, (uint8_t *)device_name, NULL);
  }

  if (strlen(device_name) == 0) {
    strcpy(device_name, "Unknown Device");
  }

  if (discovered_devices_count < MAX_DISCOVERED_DEVICES)
  {
    bool duplicate = false;
    for (int i = 0; i < discovered_devices_count; i++) {
      if (memcmp(discovered_devices[i].bda, param->disc_res.bda, ESP_BD_ADDR_LEN) == 0) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      memcpy(discovered_devices[discovered_devices_count].bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
      strncpy(discovered_devices[discovered_devices_count].bdname, device_name, ESP_BT_GAP_MAX_BDNAME_LEN);
      discovered_devices[discovered_devices_count].is_used = true;
      discovered_devices_count++;
      ESP_LOGI(BT_AV_TAG, "Discovered device: %s (%s)", bda_str, device_name);
    }
  }
}

static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
  switch (event)
  {
  case ESP_BT_GAP_DISC_RES_EVT:
  {
    if (s_a2d_state == APP_AV_STATE_DISCOVERING) { filter_inquiry_scan_result(param); }
    break;
  }
  case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
  {
    if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
      ESP_LOGI(BT_AV_TAG, "Device discovery stopped.");
      if (is_manual_connect_requested && s_a2d_state == APP_AV_STATE_DISCOVERED) {
        s_a2d_state = APP_AV_STATE_CONNECTING;
        ESP_LOGI(BT_AV_TAG, "a2dp connecting to manual peer.");
        esp_a2d_source_connect(s_peer_bda);
      } else {
        s_a2d_state = APP_AV_STATE_IDLE;
        ESP_LOGI(BT_AV_TAG, "Scan finished. Idle state.");
      }
    } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
      ESP_LOGI(BT_AV_TAG, "Discovery started.");
    }
    break;
  }
  case ESP_BT_GAP_AUTH_CMPL_EVT:
  {
    if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
      ESP_LOGI(BT_AV_TAG, "[BT] ✓ Pairing success with: %s", param->auth_cmpl.device_name);
    } else {
      ESP_LOGE(BT_AV_TAG, "[BT] Pairing failed with status: %d", param->auth_cmpl.stat);
    }
    break;
  }
  case ESP_BT_GAP_PIN_REQ_EVT:
  {
    if (param->pin_req.min_16_digit) {
      ESP_LOGI(BT_AV_TAG, "[BT] PIN requested (16-digit). Replying with zeros.");
      esp_bt_pin_code_t pin_code = {0};
      esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
    } else {
      ESP_LOGI(BT_AV_TAG, "[BT] PIN requested. Replying with 0000.");
      esp_bt_pin_code_t pin_code;
      pin_code[0] = '0'; pin_code[1] = '0'; pin_code[2] = '0'; pin_code[3] = '0';
      esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
    }
    break;
  }
#if (CONFIG_EXAMPLE_SSP_ENABLED == true)
  case ESP_BT_GAP_CFM_REQ_EVT:
    ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %06" PRIu32, param->cfm_req.num_val);
    esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
    break;
  case ESP_BT_GAP_KEY_NOTIF_EVT:
    ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey: %06" PRIu32, param->key_notif.passkey);
    break;
  case ESP_BT_GAP_KEY_REQ_EVT:
    ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
    break;
#endif
  case ESP_BT_GAP_MODE_CHG_EVT:
    /* Power mode change (active/sniff/park) - suppress noisy log */
    break;
  case ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT:
    if (param->get_dev_name_cmpl.status == ESP_BT_STATUS_SUCCESS) {
      ESP_LOGI(BT_AV_TAG, "[BT] Local device name: %s", param->get_dev_name_cmpl.name);
    } else {
      ESP_LOGW(BT_AV_TAG, "[BT] Failed to get local device name");
    }
    break;
  default:
    ESP_LOGI(BT_AV_TAG, "event: %d", event);
    break;
  }
  return;
}

static void bt_av_hdl_stack_evt(uint16_t event, void *p_param)
{
  ESP_LOGD(BT_AV_TAG, "%s event: %d", __func__, event);
  switch (event)
  {
  case BT_APP_STACK_UP_EVT:
  {
    char *dev_name = LOCAL_DEVICE_NAME;
    esp_bt_gap_set_device_name(dev_name);
    
    /* Set Class of Device to Audio/Video Source to prevent rejection by some headphones */
    esp_bt_cod_t cod;
    cod.major = ESP_BT_COD_MAJOR_DEV_AV;
    cod.minor = 0;
    cod.service = ESP_BT_COD_SRVC_AUDIO;
    esp_bt_gap_set_cod(cod, ESP_BT_INIT_COD);
    
    esp_bt_gap_register_callback(bt_app_gap_cb);

    esp_avrc_ct_init();
    esp_avrc_ct_register_callback(bt_app_rc_ct_cb);

    esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
    ESP_ERROR_CHECK(esp_avrc_tg_set_rn_evt_cap(&evt_set));

    esp_a2d_source_init();
    esp_a2d_register_callback(&bt_app_a2d_cb);
    esp_a2d_source_register_data_callback(bt_app_a2d_data_cb);

    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    esp_bt_gap_get_device_name();

    ESP_LOGI(BT_AV_TAG, "Stack up. Checking NVS for saved BT device...");
    if (load_bt_device_from_nvs(s_peer_bda, (char *)s_peer_bdname)) {
      ESP_LOGI(BT_AV_TAG, "Found saved device: %s. Deferring connection until Time Sync...", s_peer_bdname);
      s_bt_retry_count = 0;
      s_a2d_state = APP_AV_STATE_UNCONNECTED;
    } else {
      ESP_LOGI(BT_AV_TAG, "No saved BT device. Waiting for manual scan request.");
    }

    do {
      int tmr_id = 0;
      s_tmr = xTimerCreate("connTmr", (10000 / portTICK_PERIOD_MS), pdTRUE, (void *)&tmr_id, bt_app_a2d_heart_beat);
      xTimerStart(s_tmr, portMAX_DELAY);
    } while (0);
    break;
  }
  default:
    ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
    break;
  }
}

static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
  bt_app_work_dispatch(bt_app_av_sm_hdlr, event, param, sizeof(esp_a2d_cb_param_t), NULL);
}

static void bt_app_a2d_heart_beat(TimerHandle_t arg)
{
  bt_app_work_dispatch(bt_app_av_sm_hdlr, BT_APP_HEART_BEAT_EVT, NULL, 0, NULL);
}

static void bt_app_av_sm_hdlr(uint16_t event, void *param)
{
  /* Suppress noisy heartbeat logs; only log if an interesting event occurs */
  switch (s_a2d_state)
  {
  case APP_AV_STATE_DISCOVERING:
  case APP_AV_STATE_DISCOVERED:
    break;
  case APP_AV_STATE_UNCONNECTED:
    bt_app_av_state_unconnected_hdlr(event, param);
    break;
  case APP_AV_STATE_CONNECTING:
    bt_app_av_state_connecting_hdlr(event, param);
    break;
  case APP_AV_STATE_CONNECTED:
    bt_app_av_state_connected_hdlr(event, param);
    break;
  case APP_AV_STATE_DISCONNECTING:
    bt_app_av_state_disconnecting_hdlr(event, param);
    break;
  default:
    ESP_LOGE(BT_AV_TAG, "%s invalid state: %d", __func__, s_a2d_state);
    break;
  }
}

static void bt_app_av_state_unconnected_hdlr(uint16_t event, void *param)
{
  esp_a2d_cb_param_t *a2d = NULL;
  switch (event)
  {
  case ESP_A2D_CONNECTION_STATE_EVT:
  case ESP_A2D_AUDIO_STATE_EVT:
  case ESP_A2D_AUDIO_CFG_EVT:
  case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    break;
  case BT_APP_HEART_BEAT_EVT:
  {
    if (!Sys_Info.isTimeSync) {
        ESP_LOGW(BT_AV_TAG, "[BT] Waiting for NTP time sync before connecting...");
        break;
    }
    char bda_str[18];
    ESP_LOGI(BT_AV_TAG, "[BT] Connecting to saved device: %s [%s]", s_peer_bdname, bda2str(s_peer_bda, bda_str, sizeof(bda_str)));
    esp_a2d_source_connect(s_peer_bda);
    s_a2d_state = APP_AV_STATE_CONNECTING;
    s_connecting_intv = 0;
    break;
  }
  case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT:
  {
    a2d = (esp_a2d_cb_param_t *)(param);
    ESP_LOGI(BT_AV_TAG, "%s, delay value: %u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
    break;
  }
  default:
    ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
    break;
  }
}

static void bt_app_av_state_connecting_hdlr(uint16_t event, void *param)
{
  esp_a2d_cb_param_t *a2d = NULL;
  switch (event)
  {
  case ESP_A2D_CONNECTION_STATE_EVT:
  {
    a2d = (esp_a2d_cb_param_t *)(param);
    if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
      ESP_LOGI(BT_AV_TAG, "[BT] ✓ Connected to: %s", s_peer_bdname);
      s_bt_retry_count = 0;
      s_a2d_state = APP_AV_STATE_CONNECTED;
      s_media_state = APP_AV_MEDIA_STATE_IDLE;
    } else if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
      if (s_bt_retry_count < 10) {
        s_bt_retry_count++;
        ESP_LOGI(BT_AV_TAG, "Connection failed. Retrying connect to %s %d/10...", s_peer_bdname, s_bt_retry_count);
        esp_a2d_source_connect(s_peer_bda);
        s_a2d_state = APP_AV_STATE_CONNECTING;
      } else {
        ESP_LOGE(BT_AV_TAG, "Failed to connect to %s after 10 retries.", s_peer_bdname);
        s_bt_retry_count = 0;
        s_a2d_state = APP_AV_STATE_IDLE;
      }
    }
    break;
  }
  case ESP_A2D_AUDIO_STATE_EVT:
  case ESP_A2D_AUDIO_CFG_EVT:
  case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    break;
  case BT_APP_HEART_BEAT_EVT:
    if (++s_connecting_intv >= 2) {
      s_a2d_state = APP_AV_STATE_UNCONNECTED;
      s_connecting_intv = 0;
    }
    break;
  case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT:
  {
    a2d = (esp_a2d_cb_param_t *)(param);
    ESP_LOGI(BT_AV_TAG, "%s, delay value: %u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
    break;
  }
  default:
    ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
    break;
  }
}

static void bt_app_av_media_proc(uint16_t event, void *param)
{
  esp_a2d_cb_param_t *a2d = NULL;
  switch (s_media_state)
  {
  case APP_AV_MEDIA_STATE_IDLE:
  {
    if (event == BT_APP_HEART_BEAT_EVT) {
      ESP_LOGD(BT_AV_TAG, "[BT] Checking A2DP media source readiness...");
      esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
    } else if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
      a2d = (esp_a2d_cb_param_t *)(param);
      if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY && a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
        ESP_LOGI(BT_AV_TAG, "[BT] Media source ready. Starting audio stream...");
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        s_media_state = APP_AV_MEDIA_STATE_STARTING;
      }
    }
    break;
  }
  case APP_AV_MEDIA_STATE_STARTING:
  {
    if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
      a2d = (esp_a2d_cb_param_t *)(param);
      if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START && a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
        ESP_LOGI(BT_AV_TAG, "[BT] Audio stream started successfully.");
        s_intv_cnt = 0;
        s_media_state = APP_AV_MEDIA_STATE_STARTED;
      } else {
        ESP_LOGW(BT_AV_TAG, "[BT] Audio stream start failed. Will retry.");
        s_media_state = APP_AV_MEDIA_STATE_IDLE;
      }
    }
    break;
  }
  case APP_AV_MEDIA_STATE_STARTED:
    break;
  case APP_AV_MEDIA_STATE_STOPPING:
  {
    if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
      a2d = (esp_a2d_cb_param_t *)(param);
      if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_SUSPEND && a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
        ESP_LOGI(BT_AV_TAG, "[BT] Audio stream suspended. Disconnecting...");
        s_media_state = APP_AV_MEDIA_STATE_IDLE;
        esp_a2d_source_disconnect(s_peer_bda);
        s_a2d_state = APP_AV_STATE_DISCONNECTING;
      } else {
        ESP_LOGI(BT_AV_TAG, "[BT] Suspending audio stream...");
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
      }
    }
    break;
  }
  default:
    break;
  }
}

static void bt_app_av_state_connected_hdlr(uint16_t event, void *param)
{
  esp_a2d_cb_param_t *a2d = NULL;
  switch (event)
  {
  case ESP_A2D_CONNECTION_STATE_EVT:
  {
    a2d = (esp_a2d_cb_param_t *)(param);
    if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
      ESP_LOGW(BT_AV_TAG, "[BT] Disconnected from: %s", s_peer_bdname);
      s_a2d_state = APP_AV_STATE_UNCONNECTED;
    }
    break;
  }
  case ESP_A2D_AUDIO_STATE_EVT:
  {
    a2d = (esp_a2d_cb_param_t *)(param);
    if (ESP_A2D_AUDIO_STATE_STARTED == a2d->audio_stat.state) { s_pkt_cnt = 0; }
    break;
  }
  case ESP_A2D_AUDIO_CFG_EVT:
    break;
  case ESP_A2D_MEDIA_CTRL_ACK_EVT:
  case BT_APP_HEART_BEAT_EVT:
  {
    bt_app_av_media_proc(event, param);
    break;
  }
  case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT:
  {
    a2d = (esp_a2d_cb_param_t *)(param);
    ESP_LOGI(BT_AV_TAG, "%s, delay value: %u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
    break;
  }
  default:
    ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
    break;
  }
}

static void bt_app_av_state_disconnecting_hdlr(uint16_t event, void *param)
{
  esp_a2d_cb_param_t *a2d = NULL;
  switch (event)
  {
  case ESP_A2D_CONNECTION_STATE_EVT:
  {
    a2d = (esp_a2d_cb_param_t *)(param);
    if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
      ESP_LOGI(BT_AV_TAG, "[BT] Disconnection complete.");
      s_a2d_state = APP_AV_STATE_UNCONNECTED;
    }
    break;
  }
  case ESP_A2D_AUDIO_STATE_EVT:
  case ESP_A2D_AUDIO_CFG_EVT:
  case ESP_A2D_MEDIA_CTRL_ACK_EVT:
  case BT_APP_HEART_BEAT_EVT:
    break;
  case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT:
  {
    a2d = (esp_a2d_cb_param_t *)(param);
    ESP_LOGI(BT_AV_TAG, "%s, delay value: 0x%u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
    break;
  }
  default:
    ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
    break;
  }
}

static void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
  switch (event)
  {
  case ESP_AVRC_CT_CONNECTION_STATE_EVT:
  case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
  case ESP_AVRC_CT_METADATA_RSP_EVT:
  case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
  case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
  case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
  case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
  {
    bt_app_work_dispatch(bt_av_hdl_avrc_ct_evt, event, param, sizeof(esp_avrc_ct_cb_param_t), NULL);
    break;
  }
  default:
    ESP_LOGE(BT_RC_CT_TAG, "Invalid AVRC event: %d", event);
    break;
  }
}

static void bt_av_volume_changed(void)
{
  if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_avrc_peer_rn_cap, ESP_AVRC_RN_VOLUME_CHANGE)) {
    esp_avrc_ct_send_register_notification_cmd(APP_RC_CT_TL_RN_VOLUME_CHANGE, ESP_AVRC_RN_VOLUME_CHANGE, 0);
  }
}

void bt_av_notify_evt_handler(uint8_t event_id, esp_avrc_rn_param_t *event_parameter)
{
  switch (event_id)
  {
  case ESP_AVRC_RN_VOLUME_CHANGE:
  {
    ESP_LOGI(BT_RC_CT_TAG, "Volume changed: %d", event_parameter->volume);
    ESP_LOGI(BT_RC_CT_TAG, "Set absolute volume: volume %d", event_parameter->volume + 5);
    esp_avrc_ct_send_set_absolute_volume_cmd(APP_RC_CT_TL_RN_VOLUME_CHANGE, event_parameter->volume + 5);
    bt_av_volume_changed();
    break;
  }
  default:
    break;
  }
}

static void bt_av_hdl_avrc_ct_evt(uint16_t event, void *p_param)
{
  ESP_LOGD(BT_RC_CT_TAG, "%s evt %d", __func__, event);
  esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)(p_param);
  switch (event)
  {
  case ESP_AVRC_CT_CONNECTION_STATE_EVT:
  {
    uint8_t *bda = rc->conn_stat.remote_bda;
    ESP_LOGI(BT_RC_CT_TAG, "[AVRC] Remote control %s: [%02x:%02x:%02x:%02x:%02x:%02x]",
             rc->conn_stat.connected ? "connected" : "disconnected",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    if (rc->conn_stat.connected) {
      esp_avrc_ct_send_get_rn_capabilities_cmd(APP_RC_CT_TL_GET_CAPS);
    } else {
      s_avrc_peer_rn_cap.bits = 0;
    }
    break;
  }
  case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
    ESP_LOGI(BT_RC_CT_TAG, "AVRC passthrough response: key_code 0x%x, key_state %d, rsp_code %d", rc->psth_rsp.key_code, rc->psth_rsp.key_state, rc->psth_rsp.rsp_code);
    break;
  case ESP_AVRC_CT_METADATA_RSP_EVT:
    ESP_LOGI(BT_RC_CT_TAG, "AVRC metadata response: attribute id 0x%x, %s", rc->meta_rsp.attr_id, rc->meta_rsp.attr_text);
    free(rc->meta_rsp.attr_text);
    break;
  case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
    ESP_LOGI(BT_RC_CT_TAG, "AVRC event notification: %d", rc->change_ntf.event_id);
    bt_av_notify_evt_handler(rc->change_ntf.event_id, &rc->change_ntf.event_parameter);
    break;
  case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
    ESP_LOGI(BT_RC_CT_TAG, "AVRC remote features %" PRIx32 ", TG features %x", rc->rmt_feats.feat_mask, rc->rmt_feats.tg_feat_flag);
    break;
  case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
    ESP_LOGI(BT_RC_CT_TAG, "remote rn_cap: count %d, bitmask 0x%x", rc->get_rn_caps_rsp.cap_count, rc->get_rn_caps_rsp.evt_set.bits);
    s_avrc_peer_rn_cap.bits = rc->get_rn_caps_rsp.evt_set.bits;
    bt_av_volume_changed();
    break;
  case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
    ESP_LOGI(BT_RC_CT_TAG, "Set absolute volume response: volume %d", rc->set_volume_rsp.volume);
    break;
  default:
    ESP_LOGE(BT_RC_CT_TAG, "%s unhandled event: %d", __func__, event);
    break;
  }
}

static int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len)
{
  if (data == NULL || len < 0) { return 0; }
  if (!is_playing || psram_audio_buffer == NULL)
  {
    memset(data, 0, len);
    return len;
  }

  size_t remain = psram_audio_len - psram_audio_pos;
  if (remain >= len) {
    memcpy(data, psram_audio_buffer + psram_audio_pos, len);
    psram_audio_pos += len;
  } else if (remain > 0) {
    memcpy(data, psram_audio_buffer + psram_audio_pos, remain);
    memset(data + remain, 0, len - remain);
    psram_audio_pos += remain;
    is_playing = false; // finished playing
    ESP_LOGW("PLAYER", "Finished playing audio. Waiting for next command.");
  } else {
    memset(data, 0, len);
    is_playing = false;
  }

  return len;
}

void user_bluetooth_init(void)
{
  if (is_bt_initialized) {
      ESP_LOGI(BT_AV_TAG, "Bluetooth already initialized");
      return;
  }
  is_bt_initialized = true;

  esp_err_t ret;
  ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
    ESP_LOGE(BT_AV_TAG, "%s initialize controller failed", __func__);
    return;
  }

  if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
    ESP_LOGE(BT_AV_TAG, "%s enable controller failed", __func__);
    return;
  }

  esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
  bluedroid_cfg.ssp_en = true; /* Force enable SSP for modern headsets */
  
  if ((ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg)) != ESP_OK) {
    ESP_LOGE(BT_AV_TAG, "%s initialize bluedroid failed: %s", __func__, esp_err_to_name(ret));
    return;
  }

  if (esp_bluedroid_enable() != ESP_OK) {
    ESP_LOGE(BT_AV_TAG, "%s enable bluedroid failed", __func__);
    return;
  }

  esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
  esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE; /* No Input No Output for headless device */
  esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
  /*
   * Set default parameters for Legacy Pairing
   * Use variable pin, input pin code when pairing
   */
  esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
  esp_bt_pin_code_t pin_code;
  esp_bt_gap_set_pin(pin_type, 0, pin_code);

  char bda_str[18] = {0};
  ESP_LOGI(BT_AV_TAG, "Own address:[%s]", bda2str((uint8_t *)esp_bt_dev_get_address(), bda_str, sizeof(bda_str)));

  bt_app_task_start_up();
  bt_app_work_dispatch(bt_av_hdl_stack_evt, BT_APP_STACK_UP_EVT, NULL, 0, NULL);
}
void user_bluetooth_deinit(void)
{
  if (!is_bt_initialized) {
    ESP_LOGW(BT_AV_TAG, "Bluetooth not initialized, skipping deinit.");
    return;
  }

  ESP_LOGW(BT_AV_TAG, ">>> STEP 0: Forcing Disconnect from remote speakers...");
  /* Try to disconnect any active A2DP connection first */
  esp_a2d_source_disconnect(s_peer_bda);
  vTaskDelay(pdMS_TO_TICKS(500));

  ESP_LOGW(BT_AV_TAG, ">>> STEP 1: Stopping Heartbeat Timer...");
  if (s_tmr != NULL) {
    xTimerStop(s_tmr, portMAX_DELAY);
    xTimerDelete(s_tmr, portMAX_DELAY);
    s_tmr = NULL;
  }

  ESP_LOGW(BT_AV_TAG, ">>> STEP 2: Deinitializing A2DP and AVRC...");
  esp_a2d_source_deinit();
  esp_avrc_ct_deinit();
  vTaskDelay(pdMS_TO_TICKS(300));

  ESP_LOGW(BT_AV_TAG, ">>> STEP 3: Disabling Bluedroid Stack...");
  /* esp_bluedroid_disable is a heavy blocking call */
  esp_err_t ret = esp_bluedroid_disable();
  if (ret != ESP_OK) {
    ESP_LOGE(BT_AV_TAG, "esp_bluedroid_disable failed: %s", esp_err_to_name(ret));
  }
  vTaskDelay(pdMS_TO_TICKS(500));

  ESP_LOGW(BT_AV_TAG, ">>> STEP 4: Deinitializing Bluedroid...");
  esp_bluedroid_deinit();
  vTaskDelay(pdMS_TO_TICKS(300));

  ESP_LOGW(BT_AV_TAG, ">>> STEP 5: Disabling BT Controller...");
  esp_bt_controller_disable();
  vTaskDelay(pdMS_TO_TICKS(500));

  ESP_LOGW(BT_AV_TAG, ">>> STEP 6: Deinitializing BT Controller...");
  esp_bt_controller_deinit();

  ESP_LOGW(BT_AV_TAG, ">>> STEP 7: Shutting down App Task and Queues...");
  bt_app_task_shut_down();

  is_bt_initialized = false;
  ESP_LOGW(BT_AV_TAG, ">>> Bluetooth stack shut down complete. HEAP freed.");
}
