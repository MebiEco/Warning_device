/*******************************************************************************
 * FILE: user_http_server.c
 * MÔ TẢ: Web server cho ESP32 - Giao diện cấu hình thiết bị
 * 
 * CHỨC NĂNG CHÍNH:
 *   1. Trang web 3 phần: WiFi / Azure IoT Hub / Bluetooth Speaker
 *   2. Lưu/đọc cấu hình WiFi và Azure vào NVS (bộ nhớ không bay hơi)
 *   3. API endpoints cho frontend gọi:
 *      - GET  /api/config        → Lấy cấu hình đã lưu
 *      - GET  /api/wifi/scan     → Quét mạng WiFi xung quanh
 *      - POST /api/wifi/save     → Lưu WiFi mới + kết nối lại
 *      - POST /api/wifi/clear    → Xóa WiFi đã lưu
 *      - GET  /api/wifi/status   → Kiểm tra trạng thái WiFi
 *      - POST /api/azure/save    → Lưu Azure config mới
 *      - POST /api/azure/clear   → Xóa Azure config
 *      - GET  /api/azure/status  → Kiểm tra trạng thái Azure
 *      - GET  /api/scan          → Quét thiết bị Bluetooth
 *      - POST /api/connect       → Kết nối Bluetooth speaker
 * 
 * TRUY CẬP TRANG WEB:
 *   - Qua SoftAP: http://192.168.4.1 (luôn hoạt động)
 *   - Qua WiFi:   http://<IP của ESP32>
 ******************************************************************************/

#include "user_http_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include <stdlib.h>
#include <sys/param.h>
#include "time.h"
#include "string.h"
#include "cJSON.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "user_system.h"
#include "user_azure.h"

#define EXAMPLE_HTTP_QUERY_KEY_MAX_LEN  (64)
static const char *TAG = "HTTP_SRV";

/* Variables are now extern, defined in user_excute_audio.c */

/*******************************************************************************
 * PHẦN 1: HÀM LƯU/ĐỌC WIFI TỪ NVS
 * NVS Namespace: "wifi_cfg"
 * Keys: "ssid" và "pass"
 * 
 * Được gọi bởi:
 *   - wifi_init_apsta() trong main.c (đọc khi khởi động)
 *   - wifi_save_post_handler() (lưu khi user bấm Save trên web)
 ******************************************************************************/
#define NVS_WIFI_NAMESPACE "wifi_cfg"
#define NVS_WIFI_SSID_KEY  "ssid"
#define NVS_WIFI_PASS_KEY  "pass"

void save_wifi_to_nvs(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_WIFI_SSID_KEY, ssid);
        nvs_set_str(h, NVS_WIFI_PASS_KEY, pass);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "WiFi credentials saved to NVS");
    }
}

bool load_wifi_from_nvs(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_WIFI_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t e1 = nvs_get_str(h, NVS_WIFI_SSID_KEY, ssid, &ssid_len);
    esp_err_t e2 = nvs_get_str(h, NVS_WIFI_PASS_KEY, pass, &pass_len);
    nvs_close(h);
    return (e1 == ESP_OK && e2 == ESP_OK);
}

void clear_wifi_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "WiFi NVS cleared");
    }
}

/*******************************************************************************
 * PHẦN 2: HÀM LƯU/ĐỌC AZURE TỪ NVS
 * NVS Namespace: "azure_cfg"
 * Keys: "host", "devid", "skey"
 * 
 * Được gọi bởi:
 *   - User_System_Get_Config() trong user_system.c (đọc khi khởi động)
 *   - azure_save_post_handler() (lưu khi user bấm Save trên web)
 ******************************************************************************/
#define NVS_AZURE_NAMESPACE "azure_cfg"
#define NVS_AZURE_HOST_KEY  "host"
#define NVS_AZURE_DEVID_KEY "devid"
#define NVS_AZURE_SKEY_KEY  "skey"

void save_azure_to_nvs(const char *host, const char *devid, const char *skey)
{
    nvs_handle_t h;
    if (nvs_open(NVS_AZURE_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_AZURE_HOST_KEY, host);
        nvs_set_str(h, NVS_AZURE_DEVID_KEY, devid);
        nvs_set_str(h, NVS_AZURE_SKEY_KEY, skey);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Azure credentials saved to NVS");
    }
}

bool load_azure_from_nvs(char *host, size_t hl, char *devid, size_t dl, char *skey, size_t sl)
{
    nvs_handle_t h;
    if (nvs_open(NVS_AZURE_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t e1 = nvs_get_str(h, NVS_AZURE_HOST_KEY, host, &hl);
    esp_err_t e2 = nvs_get_str(h, NVS_AZURE_DEVID_KEY, devid, &dl);
    esp_err_t e3 = nvs_get_str(h, NVS_AZURE_SKEY_KEY, skey, &sl);
    nvs_close(h);
    return (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK);
}

void clear_azure_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_AZURE_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Azure NVS cleared");
    }
}

/* ─────────────────────────────────────────────────────────
 *  HTML PAGE — 3 sections: WiFi · Azure IoT Hub · Bluetooth
 * ───────────────────────────────────────────────────────── */
static const char *html_page =
"<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Device Setup</title><style>"
"@import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap');"
"*{margin:0;padding:0;box-sizing:border-box}body{font-family:'Inter',sans-serif;background:linear-gradient(135deg,#e8edf5 0%,#d5dde8 50%,#c3cfe2 100%);min-height:100vh;padding:20px 10px 40px;display:flex;flex-direction:column;align-items:center}"
".header{text-align:center;margin-bottom:24px}.header h1{font-size:22px;font-weight:700;color:#3b82f6;letter-spacing:-.5px}.header p{font-size:13px;color:#6b7280;margin-top:4px}"
".card{background:#fff;border-radius:16px;padding:24px 20px;margin-bottom:18px;width:100%;max-width:420px;box-shadow:0 4px 24px rgba(0,0,0,.06),0 1px 4px rgba(0,0,0,.04)}"
".card-title{display:flex;align-items:center;gap:10px;font-size:16px;font-weight:600;color:#1e293b;margin-bottom:18px}.card-title .icon{width:32px;height:32px;border-radius:8px;display:flex;align-items:center;justify-content:center;font-size:16px}"
".icon-wifi{background:linear-gradient(135deg,#6366f1,#818cf8);color:#fff}.icon-azure{background:linear-gradient(135deg,#0ea5e9,#38bdf8);color:#fff}.icon-bt{background:linear-gradient(135deg,#3b82f6,#60a5fa);color:#fff}"
"label{display:block;font-size:12px;font-weight:500;color:#6b7280;margin-bottom:5px;margin-top:14px}label:first-of-type{margin-top:0}input,select{width:100%;padding:10px 12px;border:1.5px solid #e2e8f0;border-radius:10px;font-size:14px;font-family:inherit;color:#1e293b;background:#f8fafc;transition:border .2s,box-shadow .2s;outline:none}"
"input:focus,select:focus{border-color:#6366f1;box-shadow:0 0 0 3px rgba(99,102,241,.12)}.pass-wrap{position:relative}.pass-wrap input{padding-right:40px}.pass-toggle{position:absolute;right:10px;top:50%;transform:translateY(-50%);background:none;border:none;cursor:pointer;font-size:18px;color:#94a3b8;padding:4px}"
".btn-row{display:flex;gap:10px;margin-top:18px;flex-wrap:wrap}.btn{flex:1;min-width:100px;padding:11px 16px;border:none;border-radius:12px;font-size:14px;font-weight:600;font-family:inherit;cursor:pointer;transition:transform .15s,box-shadow .15s;text-align:center}"
".btn:active{transform:scale(.97)}.btn-red{background:linear-gradient(135deg,#ef4444,#f87171);color:#fff;box-shadow:0 3px 12px rgba(239,68,68,.25)}.btn-green{background:linear-gradient(135deg,#10b981,#34d399);color:#fff;box-shadow:0 3px 12px rgba(16,185,129,.25)}.btn-blue{background:linear-gradient(135deg,#3b82f6,#60a5fa);color:#fff;box-shadow:0 3px 12px rgba(59,130,246,.25)}"
".btn-full{flex:none;width:100%}.status-msg{text-align:center;font-size:13px;margin-top:12px;min-height:18px;color:#10b981;font-weight:500;animation:fadeIn .3s}.status-err{color:#ef4444}@keyframes fadeIn{from{opacity:0;transform:translateY(-4px)}to{opacity:1;transform:translateY(0)}}"
"#btDeviceList{list-style:none;padding:0;margin-top:14px}#btDeviceList li{padding:12px 14px;border-radius:10px;margin:6px 0;background:#f8fafc;border:1px solid #e2e8f0;display:flex;justify-content:space-between;align-items:center;transition:background .2s}"
"#btDeviceList li:hover{background:#eef2ff}#btDeviceList li .dev-name{font-weight:600;color:#1e293b;font-size:14px}#btDeviceList li .dev-mac{font-size:11px;color:#94a3b8;margin-top:2px}"
"#btDeviceList li button{padding:7px 14px;border-radius:8px;border:none;background:linear-gradient(135deg,#10b981,#34d399);color:#fff;font-size:12px;font-weight:600;cursor:pointer;transition:transform .15s}#btDeviceList li button:active{transform:scale(.95)}"
".spinner{display:inline-block;width:14px;height:14px;border:2px solid #fff;border-top-color:transparent;border-radius:50%;animation:spin .6s linear infinite;vertical-align:middle;margin-right:6px}@keyframes spin{to{transform:rotate(360deg)}}"
"</style></head><body><div class=\"header\"><h1>&#9881; Device Setup</h1><p>Configure connectivity parameters</p></div>"
"<div class=\"card\"><div class=\"card-title\"><div class=\"icon icon-wifi\">&#128246;</div> WiFi Configuration</div>"
"<label for=\"wifiSelect\">Available Networks</label><select id=\"wifiSelect\" onchange=\"document.getElementById('wifiHidden').value=this.value\"><option>-- Scan first --</option></select>"
"<label for=\"wifiHidden\">Hidden SSID (Optional)</label><input id=\"wifiHidden\" placeholder=\"Enter SSID manually\"><label for=\"wifiPass\">Password</label><div class=\"pass-wrap\"><input id=\"wifiPass\" type=\"password\" placeholder=\"WiFi password\">"
"<button class=\"pass-toggle\" onclick=\"var p=document.getElementById('wifiPass');p.type=p.type==='password'?'text':'password';this.innerHTML=p.type==='password'?'&#128065;':'&#128064;'\">&#128065;</button></div>"
"<div class=\"btn-row\"><button class=\"btn btn-red\" onclick=\"clearWifi()\">Clear WiFi</button><button class=\"btn btn-green\" onclick=\"saveWifi()\">Save WiFi Config</button></div>"
"<button class=\"btn btn-blue btn-full\" style=\"margin-top:10px\" id=\"scanWifiBtn\" onclick=\"scanWifi()\">Scan Networks</button><div id=\"wifiStatus\" class=\"status-msg\"></div></div>"
"<div class=\"card\"><div class=\"card-title\"><div class=\"icon icon-azure\">&#9729;</div> Azure IoT Hub</div>"
"<label for=\"azureHost\">Host Name</label><input id=\"azureHost\" placeholder=\"your-hub.azure-devices.net\"><label for=\"azureDevId\">Device ID</label><input id=\"azureDevId\" placeholder=\"my-device-1\">"
"<label for=\"azureKey\">Symmetric Key</label><input id=\"azureKey\" placeholder=\"Base64 symmetric key\"><div class=\"btn-row\"><button class=\"btn btn-red\" onclick=\"clearAzure()\">Clear Azure</button><button class=\"btn btn-green\" onclick=\"saveAzure()\">Save Azure Config</button></div><div id=\"azureStatus\" class=\"status-msg\"></div></div>"
"<div class=\"card\"><div class=\"card-title\"><div class=\"icon icon-bt\">&#127925;</div> Bluetooth Speaker</div>"
"<button class=\"btn btn-blue btn-full\" id=\"scanBtBtn\" onclick=\"scanBt()\">Scan Bluetooth Devices</button><div id=\"btStatus\" class=\"status-msg\"></div><ul id=\"btDeviceList\"></ul></div>"
"<script>"
"function scanWifi(){var b=document.getElementById('scanWifiBtn');b.disabled=true;b.innerHTML='<span class=\"spinner\"></span>Scanning...';document.getElementById('wifiStatus').innerText='';"
"fetch('/api/wifi/scan').then(r=>r.json()).then(d=>{var s=document.getElementById('wifiSelect');s.innerHTML='';if(d.length===0){s.innerHTML='<option>No networks found</option>';}else{d.forEach(n=>{var o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+'dBm)';s.appendChild(o);});}"
"b.disabled=false;b.innerHTML='Scan Networks';document.getElementById('wifiStatus').className='status-msg';document.getElementById('wifiStatus').innerText='Found '+d.length+' networks';}).catch(e=>{b.disabled=false;b.innerHTML='Scan Networks';document.getElementById('wifiStatus').className='status-msg status-err';document.getElementById('wifiStatus').innerText='Scan failed: '+e;});}"
"function saveWifi(){var ssid=document.getElementById('wifiHidden').value||document.getElementById('wifiSelect').value;var pass=document.getElementById('wifiPass').value;if(!ssid||ssid==='-- Scan first --'){document.getElementById('wifiStatus').className='status-msg status-err';document.getElementById('wifiStatus').innerText='Please select or enter SSID';return;}"
"fetch('/api/wifi/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid,pass:pass})}).then(r=>r.json()).then(d=>{document.getElementById('wifiStatus').className='status-msg';document.getElementById('wifiStatus').innerHTML='<span class=\"spinner\" style=\"border-color:#10b981;border-top-color:transparent\"></span> Connecting to '+ssid+'...';"
"pollWifiStatus(ssid,0);}).catch(e=>{document.getElementById('wifiStatus').className='status-msg status-err';document.getElementById('wifiStatus').innerText='Error: '+e;});}"
"function pollWifiStatus(ssid,attempt){if(attempt>=8){document.getElementById('wifiStatus').className='status-msg status-err';document.getElementById('wifiStatus').innerText='Failed to connect to '+ssid;return;}setTimeout(function(){fetch('/api/wifi/status').then(r=>r.json()).then(d=>{if(d.connected){document.getElementById('wifiStatus').className='status-msg';document.getElementById('wifiStatus').innerText='\u2705 Connected to '+ssid+'! IP: '+d.ip;}"
"else{pollWifiStatus(ssid,attempt+1);}}).catch(e=>{pollWifiStatus(ssid,attempt+1);});},2000);}"
"function clearWifi(){fetch('/api/wifi/clear',{method:'POST'}).then(r=>r.json()).then(d=>{document.getElementById('wifiStatus').className='status-msg';document.getElementById('wifiStatus').innerText=d.msg||'WiFi credentials cleared';}).catch(e=>{document.getElementById('wifiStatus').className='status-msg status-err';document.getElementById('wifiStatus').innerText='Error: '+e;});}"
"function saveAzure(){var h=document.getElementById('azureHost').value,d=document.getElementById('azureDevId').value,k=document.getElementById('azureKey').value;if(!h||!d||!k){document.getElementById('azureStatus').className='status-msg status-err';document.getElementById('azureStatus').innerText='All fields required';return;}"
"fetch('/api/azure/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({host:h,deviceId:d,key:k})}).then(r=>r.json()).then(res=>{document.getElementById('azureStatus').className='status-msg';document.getElementById('azureStatus').innerHTML=res.msg;}).catch(e=>{document.getElementById('azureStatus').className='status-msg status-err';document.getElementById('azureStatus').innerText='Error: '+e;});}"
"function clearAzure(){fetch('/api/azure/clear',{method:'POST'}).then(r=>r.json()).then(d=>{document.getElementById('azureStatus').className='status-msg';document.getElementById('azureStatus').innerText=d.msg||'Azure credentials cleared';}).catch(e=>{document.getElementById('azureStatus').className='status-msg status-err';document.getElementById('azureStatus').innerText='Error: '+e;});}"
"function scanBt(){var b=document.getElementById('scanBtBtn');b.disabled=true;b.innerHTML='<span class=\"spinner\"></span>Scanning... (~10s)';document.getElementById('btStatus').innerText='';document.getElementById('btDeviceList').innerHTML='';"
"fetch('/api/scan').then(r=>r.json()).then(d=>{b.disabled=false;b.innerHTML='Scan Bluetooth Devices';document.getElementById('btStatus').className='status-msg';document.getElementById('btStatus').innerText='Found '+d.length+' devices';var ul=document.getElementById('btDeviceList');d.forEach(dev=>{var li=document.createElement('li');"
"li.innerHTML='<div><div class=\"dev-name\">'+dev.name+'</div><div class=\"dev-mac\">'+dev.mac+'</div></div><button onclick=\"connectBt(\\''+dev.mac+'\\',this)\">Save Device</button>';ul.appendChild(li);});}).catch(e=>{b.disabled=false;b.innerHTML='Scan Bluetooth Devices';document.getElementById('btStatus').className='status-msg status-err';document.getElementById('btStatus').innerText='Scan error: '+e;});}"
"function connectBt(mac,btn){btn.disabled=true;btn.innerText='Saving...';document.getElementById('btStatus').innerText='Saving '+mac+' to NVS...';fetch('/api/connect',{method:'POST',body:JSON.stringify({mac:mac})}).then(r=>r.json()).then(d=>{btn.innerText='Saved!';document.getElementById('btStatus').className='status-msg';document.getElementById('btStatus').innerText='Device saved. Please reboot to connect.';}).catch(e=>{btn.disabled=false;btn.innerText='Save Device';document.getElementById('btStatus').className='status-msg status-err';document.getElementById('btStatus').innerText='Error: '+e;});}"
"fetch('/api/config').then(r=>r.json()).then(d=>{if(d.wifi_ssid)document.getElementById('wifiHidden').value=d.wifi_ssid;if(d.azure_host)document.getElementById('azureHost').value=d.azure_host;if(d.azure_devid)document.getElementById('azureDevId').value=d.azure_devid;if(d.azure_key)document.getElementById('azureKey').value=d.azure_key;}).catch(e=>{});"
"</script></body></html>";


/* ═══════════════════════════════════════════════════════════════
 *  HTTP HANDLERS
 * ═══════════════════════════════════════════════════════════════ */

/* GET / — serve the main page */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close");

    // Send the large HTML page in chunks to avoid EAGAIN (11) error
    size_t total_len = strlen(html_page);
    size_t offset = 0;
    const size_t chunk_size = 4096;

    while (offset < total_len) {
        size_t send_len = (total_len - offset < chunk_size) ? (total_len - offset) : chunk_size;
        esp_err_t err = httpd_resp_send_chunk(req, html_page + offset, send_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "File sending failed!");
            httpd_resp_send_chunk(req, NULL, 0); // abort
            return err;
        }
        offset += send_len;
        vTaskDelay(pdMS_TO_TICKS(10)); // Give breathing room
    }
    httpd_resp_send_chunk(req, NULL, 0); // Finish chunked response

    return ESP_OK;
}

/* GET /api/config — return current saved config as JSON */
static esp_err_t config_get_handler(httpd_req_t *req)
{
    char ssid[64] = {0}, pass[64] = {0};
    char host[64] = {0}, devid[32] = {0}, skey[128] = {0};

    load_wifi_from_nvs(ssid, sizeof(ssid), pass, sizeof(pass));
    load_azure_from_nvs(host, sizeof(host), devid, sizeof(devid), skey, sizeof(skey));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "wifi_ssid", ssid);
    cJSON_AddStringToObject(root, "azure_host", host);
    cJSON_AddStringToObject(root, "azure_devid", devid);
    cJSON_AddStringToObject(root, "azure_key", skey);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_Delete(root);
    free(json);
    return ESP_OK;
}

/* GET /api/wifi/scan — scan WiFi networks and return JSON array */
static esp_err_t wifi_scan_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Starting WiFi scan...");

    /* Start scan (blocking) */
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };
    esp_wifi_scan_start(&scan_config, true);

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 20) num = 20;

    wifi_ap_record_t *records = calloc(num, sizeof(wifi_ap_record_t));
    esp_wifi_scan_get_ap_records(&num, records);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < num; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (const char *)records[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
        cJSON_AddItemToArray(arr, item);
    }
    free(records);

    char *json = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_Delete(arr);
    free(json);
    return ESP_OK;
}

/* POST /api/wifi/save — save WiFi creds and reconnect */
static esp_err_t wifi_save_post_handler(httpd_req_t *req)
{
    char buf[256] = {0};
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ssid"));
    const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(root, "pass"));

    if (ssid) {
        save_wifi_to_nvs(ssid, pass ? pass : "");
        ESP_LOGI(TAG, "WiFi saved Configuration Only: %s", ssid);
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "{\"msg\":\"Saved WiFi Configuration successfully in NVS. Please hold Config_Button to exit config mode and connect.\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /api/wifi/status — check current STA connection status */
static esp_err_t wifi_status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", Sys_Info.isWifiConnected);

    if (Sys_Info.isWifiConnected) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info;
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char ip_str[16];
            sprintf(ip_str, IPSTR, IP2STR(&ip_info.ip));
            cJSON_AddStringToObject(root, "ip", ip_str);
        } else {
            cJSON_AddStringToObject(root, "ip", "unknown");
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_Delete(root);
    free(json);
    return ESP_OK;
}

/* POST /api/wifi/clear — clear WiFi NVS */
static esp_err_t wifi_clear_post_handler(httpd_req_t *req)
{
    clear_wifi_nvs();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "{\"msg\":\"WiFi credentials cleared\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* POST /api/azure/save — save Azure IoT Hub config */
static esp_err_t azure_save_post_handler(httpd_req_t *req)
{
    char buf[512] = {0};
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    const char *host  = cJSON_GetStringValue(cJSON_GetObjectItem(root, "host"));
    const char *devid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "deviceId"));
    const char *skey  = cJSON_GetStringValue(cJSON_GetObjectItem(root, "key"));

    if (host && devid && skey) {
        save_azure_to_nvs(host, devid, skey);
        ESP_LOGI(TAG, "Azure config saved: %s / %s", host, devid);
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "{\"msg\":\"Saved Azure Config. Please reboot to connect.\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* POST /api/azure/clear — clear Azure NVS */
static esp_err_t azure_clear_post_handler(httpd_req_t *req)
{
    clear_azure_nvs();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "{\"msg\":\"Azure credentials cleared\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /api/azure/status — check Azure connection status */
static esp_err_t azure_status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", IoTHubHandle.isAzureInitialized);
    if (IoTHubHandle.isAzureInitialized) {
        cJSON_AddStringToObject(root, "host", IoTHubHandle.hostName);
        cJSON_AddStringToObject(root, "deviceId", IoTHubHandle.deviceId);
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_Delete(root);
    free(json);
    return ESP_OK;
}

/* GET /api/scan — Bluetooth device scan (existing) */
static esp_err_t bt_scan_get_handler(httpd_req_t *req)
{
    /* Initialize Bluetooth subsystem dynamically when user scans in Config Mode */
    user_bluetooth_init();

    start_bt_device_scan();
    vTaskDelay(pdMS_TO_TICKS(11000));

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < discovered_devices_count; i++) {
        cJSON *dev = cJSON_CreateObject();
        char mac_str[18];
        sprintf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
            discovered_devices[i].bda[0], discovered_devices[i].bda[1], discovered_devices[i].bda[2],
            discovered_devices[i].bda[3], discovered_devices[i].bda[4], discovered_devices[i].bda[5]);

        cJSON_AddStringToObject(dev, "mac", mac_str);
        cJSON_AddStringToObject(dev, "name", discovered_devices[i].bdname);
        cJSON_AddItemToArray(root, dev);
    }

    char *json_response = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json_response, HTTPD_RESP_USE_STRLEN);

    cJSON_Delete(root);
    free((void *)json_response);
    return ESP_OK;
}

/* POST /api/connect — connect to Bluetooth device (existing) */
static esp_err_t bt_connect_post_handler(httpd_req_t *req)
{
    user_bluetooth_init();
    char content[100];
    size_t recv_size = MIN(req->content_len, sizeof(content) - 1);
    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) return ESP_FAIL;
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root) {
        cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
        if (mac_item && cJSON_IsString(mac_item)) {
            const char *mac_str = mac_item->valuestring;
            esp_bd_addr_t bda;
            int mac[6];
            sscanf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
            for(int i=0; i<6; i++) bda[i] = (uint8_t)mac[i];

            const char *device_name = "Unrecognized Device";
            for (int i = 0; i < discovered_devices_count; i++) {
                if (memcmp(discovered_devices[i].bda, bda, ESP_BD_ADDR_LEN) == 0) {
                    device_name = discovered_devices[i].bdname;
                    break;
                }
            }
            connect_to_bt_device(bda, device_name);
        }
        cJSON_Delete(root);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* POST /api/control — device control (existing) */
esp_err_t device_control_post_handler(httpd_req_t *req)
{
    char resp[256] = {0};
    char content[256];
    size_t recv_size = MIN(req->content_len, sizeof(content));

    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) return ESP_FAIL;
    content[ret] = '\0';

    if(strcmp(content, "AirBlower1") == 0)
    {
        ESP_LOGI("SERVER", "------------------------------");
        ESP_LOGW("SERVER", "AirBlower 1 warning\n\n\n");
        file_to_play = 1;
    }else if(strcmp(content, "AirBlower2") == 0){
        ESP_LOGI("SERVER", "------------------------------");
        ESP_LOGW("SERVER", "AirBlower 2 warning\n\n");
        file_to_play = 2;
    }

    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


/* ═══════════════════════════════════════════════════════════════
 *  URI definitions
 * ═══════════════════════════════════════════════════════════════ */

static const httpd_uri_t uri_root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
    .user_ctx  = NULL
};

httpd_uri_t uri_control = {
    .uri      = "/api/control",
    .method   = HTTP_POST,
    .handler  = device_control_post_handler,
    .user_ctx = NULL
};


esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    if (strcmp("/hello", req->uri) == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "/hello URI is not available");
        return ESP_OK;
    } else if (strcmp("/echo", req->uri) == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "/echo URI is not available");
        return ESP_FAIL;
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Some 404 error message");
    return ESP_FAIL;
}


static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.lru_purge_enable = true;
    config.max_open_sockets = 5;
    config.max_uri_handlers = 16;
    config.stack_size = 8192;

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK)
    {
        ESP_LOGI(TAG, "Registering URI handlers");

        /* Root page */
        httpd_register_uri_handler(server, &uri_root);

        /* Control */
        httpd_register_uri_handler(server, &uri_control);

        /* Config (load saved values) */
        httpd_uri_t cfg_uri = { .uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &cfg_uri);

        /* WiFi scan / save / clear */
        httpd_uri_t ws_uri = { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &ws_uri);

        httpd_uri_t wsave_uri = { .uri = "/api/wifi/save", .method = HTTP_POST, .handler = wifi_save_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &wsave_uri);

        httpd_uri_t wclear_uri = { .uri = "/api/wifi/clear", .method = HTTP_POST, .handler = wifi_clear_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &wclear_uri);

        httpd_uri_t wstatus_uri = { .uri = "/api/wifi/status", .method = HTTP_GET, .handler = wifi_status_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &wstatus_uri);

        /* Azure save / clear */
        httpd_uri_t asave_uri = { .uri = "/api/azure/save", .method = HTTP_POST, .handler = azure_save_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &asave_uri);

        httpd_uri_t aclear_uri = { .uri = "/api/azure/clear", .method = HTTP_POST, .handler = azure_clear_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &aclear_uri);

        httpd_uri_t astatus_uri = { .uri = "/api/azure/status", .method = HTTP_GET, .handler = azure_status_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &astatus_uri);

        /* Bluetooth scan & connect */
        httpd_uri_t scan_uri = { .uri = "/api/scan", .method = HTTP_GET, .handler = bt_scan_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &scan_uri);

        httpd_uri_t connect_uri = { .uri = "/api/connect", .method = HTTP_POST, .handler = bt_connect_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &connect_uri);

        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

#if !CONFIG_IDF_TARGET_LINUX
static esp_err_t stop_webserver(httpd_handle_t server)
{
    return httpd_stop(server);
}

static void disconnect_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server) {
        ESP_LOGI(TAG, "Stopping webserver");
        if (stop_webserver(*server) == ESP_OK) {
            *server = NULL;
        } else {
            ESP_LOGE(TAG, "Failed to stop http server");
        }
    }
}

static void connect_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        ESP_LOGI(TAG, "Starting webserver");
        *server = start_webserver();
    }
}
#endif // !CONFIG_IDF_TARGET_LINUX


void User_Http_Server_Task(void)
{
    /* Start immediately — SoftAP is always available at 192.168.4.1 */
    ESP_LOGI("HTTP SERVER", "Starting HTTP server (AP: 192.168.4.1)");
    start_webserver();

    while(1)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
