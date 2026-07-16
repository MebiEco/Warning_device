/*******************************************************************************
 * FILE: user_system.c
 * MÔ TẢ: Khởi tạo cấu hình hệ thống (Azure IoT Hub)
 * 
 * CHỨC NĂNG CHÍNH:
 *   - Đọc cấu hình Azure từ NVS (bộ nhớ không bay hơi)
 *   - Nếu NVS trống → dùng giá trị mặc định (hardcode)
 *   - Kiểm tra trạng thái WiFi và đồng bộ thời gian
 * 
 * GHI CHÚ:
 *   - Hàm load_azure_from_nvs() được định nghĩa trong user_http_server.c
 *   - Có thể thay đổi Azure config từ trang web (http://192.168.4.1)
 ******************************************************************************/

#include "user_system.h"
#include "esp_log.h"
#include "string.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "user_azure.h"

/* Biến toàn cục chứa thông tin hệ thống (WiFi, thời gian, v.v.) */
Sys_Info_Handle_t Sys_Info;

/* Khai báo hàm đọc NVS từ file user_http_server.c */
extern bool load_wifi_from_nvs(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
extern bool load_azure_from_nvs(char *host, size_t hl, char *devid, size_t dl, char *skey, size_t sl);

/*******************************************************************************
 * HÀM: User_System_Get_Config
 * MÔ TẢ: Đọc cấu hình Azure IoT Hub
 *   - Ưu tiên đọc từ NVS (đã lưu từ trang web)
 *   - Nếu NVS trống → dùng giá trị mặc định
 * 
 * ĐỂ THAY ĐỔI GIÁ TRỊ MẶC ĐỊNH:
 *   Sửa các dòng memcpy bên dưới (hostName, deviceId, symmetricKey)
 ******************************************************************************/
void User_System_Get_Config(void) 
{
  memset(&IoTHubHandle, 0, sizeof(IoTHubHandle));

  ESP_LOGI("SYS INIT", "Get Tcp parameters");

  /* Thử đọc cấu hình Azure đã lưu trong NVS */
  char host[64] = {0}, devid[32] = {0}, skey[128] = {0};
  if (load_azure_from_nvs(host, sizeof(host), devid, sizeof(devid), skey, sizeof(skey))) {
    /* Đã tìm thấy cấu hình trong NVS → sử dụng */
    ESP_LOGI("SYS INIT", "Loaded Azure config from NVS: %s / %s", host, devid);
    memcpy(IoTHubHandle.hostName, host, strlen(host));
    memcpy(IoTHubHandle.deviceId, devid, strlen(devid));
    memcpy(IoTHubHandle.symmetricKey, skey, strlen(skey));
  } else {
    /* NVS trống → dùng giá trị mặc định (có thể sửa ở đây) */
    ESP_LOGW("SYS INIT", "No Azure NVS config found, using defaults");
    memcpy(IoTHubHandle.hostName, "dev-iot-hub-1.azure-devices.net", strlen("dev-iot-hub-1.azure-devices.net"));
    memcpy(IoTHubHandle.deviceId, "test-specker-devices-1", strlen("test-specker-devices-1"));
    memcpy(IoTHubHandle.symmetricKey, "MG32gfQsldLSRq3krqwHa1H1wtyGP2XLToXj9b3qwG0=", strlen("MG32gfQsldLSRq3krqwHa1H1wtyGP2XLToXj9b3qwG0="));
  }
}

/* Kiểm tra thời gian đã đồng bộ chưa */
bool Is_System_Time_Synchronized(void) { return Sys_Info.isTimeSync; }

/* Kiểm tra WiFi đã kết nối internet chưa */
bool Is_System_Internet_Connected(void) { return Sys_Info.isWifiConnected; }

/*******************************************************************************
 * HÀM: User_System_Init
 * MÔ TẢ: Khởi tạo hệ thống - gọi từ app_main()
 ******************************************************************************/
void User_System_Init(void) 
{
  ESP_LOGI("SYS INIT", "");
  User_System_Get_Config();
}