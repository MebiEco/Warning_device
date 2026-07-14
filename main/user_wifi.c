#include "user_wifi.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "string.h"
#include "user_system.h"

#define EXAMPLE_ESP_MAXIMUM_RETRY 5
#define AP_SSID         "ESP32_Speaker_Setup"
#define AP_PASS         "12345678"
#define AP_MAX_CONN     4
#define AP_CHANNEL      1

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "wifi station";
static int s_retry_num = 0;

extern bool load_wifi_from_nvs(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

static void wifi_apply_softap_config(void)
{
  wifi_config_t ap_config = {
      .ap = {
          .ssid = AP_SSID,
          .ssid_len = strlen(AP_SSID),
          .password = AP_PASS,
          .channel = AP_CHANNEL,
          .max_connection = AP_MAX_CONN,
          .authmode = WIFI_AUTH_WPA2_PSK,
      },
  };
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
  {
    esp_wifi_connect();
  }
  else if (event_base == WIFI_EVENT &&
           event_id == WIFI_EVENT_STA_DISCONNECTED)
  {
    Sys_Info.isWifiConnected = false;
    if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY)
    {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "retry to connect to the AP (%d/%d)", s_retry_num, EXAMPLE_ESP_MAXIMUM_RETRY);
    }
    else
    {
      if (s_wifi_event_group) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
      }
      ESP_LOGW(TAG, "STA connection failed.");
    }
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
  {
    wifi_event_ap_staconnected_t *evt = (wifi_event_ap_staconnected_t *)event_data;
    ESP_LOGI(TAG, "Station connected to SoftAP, AID=%d", evt->aid);
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)
  {
    wifi_event_ap_stadisconnected_t *evt = (wifi_event_ap_stadisconnected_t *)event_data;
    ESP_LOGI(TAG, "Station disconnected from SoftAP, AID=%d", evt->aid);
  }
  else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
  {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "STA got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    Sys_Info.isWifiConnected = true;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

void wifi_init_config_mode(void)
{
  s_wifi_event_group = xEventGroupCreate();
  Sys_Info.isWifiConnected = false;

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_ap();
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

  /* Phải set_mode trước set_config — gọi set_config(AP) khi mode chưa AP/APSTA sẽ fail → abort */
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  wifi_apply_softap_config();
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "CONFIG MODE ENABLED (AP+STA)");
  ESP_LOGI(TAG, "SoftAP started: %s (pass: %s)", AP_SSID, AP_PASS);
  ESP_LOGI(TAG, "Config page: http://192.168.4.1");
  ESP_LOGI(TAG, "========================================");
}

void wifi_connect_sta(const char *ssid, const char *pass)
{
  if (!ssid || strlen(ssid) == 0) {
    return;
  }

  wifi_config_t sta_config = {0};
  sta_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
  strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
  if (pass) {
    strncpy((char *)sta_config.sta.password, pass, sizeof(sta_config.sta.password) - 1);
  }

  s_retry_num = 0;
  Sys_Info.isWifiConnected = false;
  if (s_wifi_event_group) {
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
  }

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
  esp_wifi_disconnect();
  ESP_ERROR_CHECK(esp_wifi_connect());
  ESP_LOGI(TAG, "Connecting STA to: %s", ssid);
}

void wifi_init_normal_mode(void)
{
  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

  wifi_config_t sta_config = {
      .sta = {
          .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
      },
  };

  char nvs_ssid[64] = {0}, nvs_pass[64] = {0};
  if (load_wifi_from_nvs(nvs_ssid, sizeof(nvs_ssid), nvs_pass, sizeof(nvs_pass)) && strlen(nvs_ssid) > 0) {
    ESP_LOGI(TAG, "Loaded WiFi from NVS: %s", nvs_ssid);
    memcpy(sta_config.sta.ssid, nvs_ssid, strlen(nvs_ssid));
    memcpy(sta_config.sta.password, nvs_pass, strlen(nvs_pass));
  } else {
    ESP_LOGW(TAG, "No WiFi in NVS, using fallback default.");
    memcpy(sta_config.sta.ssid, "Viettel 0978565965", strlen("Viettel 0978565965"));
    memcpy(sta_config.sta.password, "Chi@0816@!!", strlen("Chi@0816@!!"));
  }

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
  ESP_ERROR_CHECK(esp_wifi_start());
  /* Tắt modem-sleep — tránh TLS/MQTT bị cắt khi STA idle lâu */
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "NORMAL MODE ENABLED (STA)");
  ESP_LOGI(TAG, "Trying STA: %s", (char *)sta_config.sta.ssid);
  ESP_LOGI(TAG, "========================================");

  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));

  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "STA connected to: %s", (char *)sta_config.sta.ssid);
  } else {
    ESP_LOGE(TAG, "STA connection failed/timeout.");
  }
}
