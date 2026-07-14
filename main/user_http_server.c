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
#include "user_wifi.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "user_system.h"
#include "user_azure.h"
#include "user_audio_files.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_coexist.h"

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
 *  Web UI: HTML nhỏ + /style.css + /app.js (tránh send EAGAIN)
 *  Audio list ở card đầu trang
 * ───────────────────────────────────────────────────────── */
static const char *html_page =
"<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Device Setup</title><link rel=\"stylesheet\" href=\"/style.css\"></head><body><div class=\"header\"><h1>Device Setup</h1><p>WiFi · Azure · Audio · Bluetooth</p></div><div class=\"card\"><div class=\"card-title\"><span class=\"icon icon-audio\">A</span> Audio Files (SD)</div><div"
" class=\"file-head\"><span id=\"audioCount\">Loading…</span><button class=\"btn btn-blue\" id=\"audioRefreshBtn\" onclick=\"refreshAudio()\">Refresh</button></div><ul id=\"audioFileList\"><li class=\"empty\">Loading file list…</li></ul><label for=\"audioUrl\">Download URL (https)</label><input id=\"audioUrl\" placeholder=\"https://....blob.core.windows.net/.../file.wav\"><label for=\"audioName\">File Name</label><input"
" id=\"audioName\" placeholder=\"alarm.wav\"><button class=\"btn btn-green btn-full\" id=\"audioDlBtn\" onclick=\"downloadAudio()\">Download to SD</button><div id=\"audioStatus\" class=\"status-msg\"></div></div><div class=\"card\"><div class=\"card-title\"><span class=\"icon icon-wifi\">W</span> WiFi</div><label>Available Networks</label><select id=\"wifiSelect\" onchange=\"document.getElementById('wifiHidden').value=th"
"is.value\"><option>-- Scan first --</option></select><label>Hidden SSID</label><input id=\"wifiHidden\" placeholder=\"SSID\"><label>Password</label><input id=\"wifiPass\" type=\"password\" placeholder=\"Password\"><div class=\"btn-row\"><button class=\"btn btn-red\" onclick=\"clearWifi()\">Clear</button><button class=\"btn btn-green\" onclick=\"saveWifi()\">Save</button></div><button class=\"btn btn-blue btn-full\" id=\""
"scanWifiBtn\" onclick=\"scanWifi()\">Scan Networks</button><div id=\"wifiStatus\" class=\"status-msg\"></div></div><div class=\"card\"><div class=\"card-title\"><span class=\"icon icon-azure\">Z</span> Azure IoT Hub</div><label>Host Name</label><input id=\"azureHost\" placeholder=\"hub.azure-devices.net\"><label>Device ID</label><input id=\"azureDevId\" placeholder=\"device-id\"><label>Symmetric Key</label><input id=\""
"azureKey\" placeholder=\"key\"><div class=\"btn-row\"><button class=\"btn btn-red\" onclick=\"clearAzure()\">Clear</button><button class=\"btn btn-green\" onclick=\"saveAzure()\">Save</button></div><div id=\"azureStatus\" class=\"status-msg\"></div></div><div class=\"card\"><div class=\"card-title\"><span class=\"icon icon-bt\">B</span> Bluetooth</div><button class=\"btn btn-blue btn-full\" id=\"scanBtBtn\" onclick=\"scanBt("
")\">Scan Bluetooth</button><div id=\"btStatus\" class=\"status-msg\"></div><ul id=\"btDeviceList\"></ul></div><script src=\"/app.js\"></script></body></html>";

static const char *css_page =
"*{margin:0;padding:0;box-sizing:border-box}body{font-family:system-ui,sans-serif;background:#d5dde8;min-height:100vh;padding:16px 10px 32px;display:flex;flex-direction:column;align-items:center}.header{text-align:center;margin-bottom:16px}.header h1{font-size:20px;color:#2563eb}.header p{font-size:12px;color:#64748b;margin-top:4px}.card{background:#fff;border-radius:12px;padding:18px 16px;margin-b"
"ottom:14px;width:100%;max-width:420px;box-shadow:0 2px 12px rgba(0,0,0,.06)}.card-title{display:flex;align-items:center;gap:8px;font-size:15px;font-weight:600;color:#1e293b;margin-bottom:12px}.icon{width:28px;height:28px;border-radius:8px;display:inline-flex;align-items:center;justify-content:center;font-size:12px;font-weight:700;color:#fff}.icon-wifi{background:#6366f1}.icon-azure{background:#0ea"
"5e9}.icon-bt{background:#3b82f6}.icon-audio{background:#f59e0b}label{display:block;font-size:12px;color:#64748b;margin:10px 0 4px}input,select{width:100%;padding:9px 10px;border:1px solid #e2e8f0;border-radius:8px;font-size:14px;background:#f8fafc}.btn-row{display:flex;gap:8px;margin-top:12px;flex-wrap:wrap}.btn{flex:1;min-width:90px;padding:10px 12px;border:none;border-radius:10px;font-size:13px;"
"font-weight:600;cursor:pointer;color:#fff}.btn-red{background:#ef4444}.btn-green{background:#10b981}.btn-blue{background:#3b82f6}.btn-full{width:100%;flex:none;margin-top:8px}.file-head{display:flex;justify-content:space-between;align-items:center;gap:8px;margin-bottom:4px}.file-head .btn{flex:none;padding:7px 12px;font-size:12px}#audioCount{font-size:12px;color:#64748b;font-weight:500}.status-msg"
"{text-align:center;font-size:12px;margin-top:10px;min-height:16px;color:#10b981}.status-err{color:#ef4444}#btDeviceList,#audioFileList{list-style:none;padding:0;margin-top:8px;max-height:280px;overflow:auto}#btDeviceList li,#audioFileList li{padding:10px 12px;border-radius:8px;margin:5px 0;background:#f8fafc;border:1px solid #e2e8f0;display:flex;justify-content:space-between;align-items:center;gap"
":8px}#audioFileList li.empty{display:block;text-align:center;color:#94a3b8;font-size:13px;border-style:dashed}.dev-name{font-weight:600;font-size:13px;color:#1e293b;word-break:break-all}.dev-mac{font-size:11px;color:#94a3b8;margin-top:2px}#btDeviceList li button,#audioFileList li button{padding:6px 12px;border:none;border-radius:8px;background:#10b981;color:#fff;font-size:12px;font-weight:600;curs"
"or:pointer}#audioFileList li button.btn-del{background:#ef4444}.spinner{display:inline-block;width:12px;height:12px;border:2px solid #fff;border-top-color:transparent;border-radius:50%;animation:spin .6s linear infinite;vertical-align:middle;margin-right:4px}@keyframes spin{to{transform:rotate(360deg)}}";

static const char *js_page =
"function setMsg(id,err,t){var e=document.getElementById(id);e.className=err?'status-msg status-err':'status-msg';e.innerText=t;}function scanWifi(){var b=document.getElementById('scanWifiBtn');b.disabled=true;b.innerHTML='<span class=\"spinner\"></span>Scanning';fetch('/api/wifi/scan').then(r=>r.json()).then(d=>{var s=document.getElementById('wifiSelect');s.innerHTML='';if(!d.length){s.innerHTML='<o"
"ption>No networks</option>';}else{d.forEach(n=>{var o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+'dBm)';s.appendChild(o);});}b.disabled=false;b.innerHTML='Scan Networks';setMsg('wifiStatus',false,'Found '+d.length+' networks');}).catch(e=>{b.disabled=false;b.innerHTML='Scan Networks';setMsg('wifiStatus',true,'Scan failed');});}function saveWifi(){var ssid=docu"
"ment.getElementById('wifiHidden').value||document.getElementById('wifiSelect').value;var pass=document.getElementById('wifiPass').value;if(!ssid||ssid.indexOf('Scan')>=0){setMsg('wifiStatus',true,'Select SSID');return;}fetch('/api/wifi/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid,pass:pass})}).then(r=>r.json()).then(function(){setMsg('wifiStatus',"
"false,'Connecting...');pollWifiStatus(ssid,0);}).catch(function(){setMsg('wifiStatus',false,'Reconnecting...');pollWifiStatus(ssid,0);});}function pollWifiStatus(ssid,a){if(a>=15){setMsg('wifiStatus',true,'Connect failed');return;}setTimeout(function(){fetch('/api/wifi/status').then(r=>r.json()).then(d=>{if(d.connected)setMsg('wifiStatus',false,'OK IP '+d.ip);else pollWifiStatus(ssid,a+1);}).catch(function(){pollWifiStatus(ssid,a+"
"1);});},2000);}function clearWifi(){fetch('/api/wifi/clear',{method:'POST'}).then(r=>r.json()).then(d=>setMsg('wifiStatus',false,d.msg||'Cleared')).catch(function(){setMsg('wifiStatus',true,'Error');});}function saveAzure(){var h=document.getElementById('azureHost').value,d=document.getElementById('azureDevId').value,k=document.getElementById('azureKey').value;if(!h||!d||!k){setMsg('azureStatus',t"
"rue,'All fields required');return;}fetch('/api/azure/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({host:h,deviceId:d,key:k})}).then(r=>r.json()).then(res=>setMsg('azureStatus',false,res.msg||'Saved')).catch(function(){setMsg('azureStatus',true,'Error');});}function clearAzure(){fetch('/api/azure/clear',{method:'POST'}).then(r=>r.json()).then(d=>setMsg('azure"
"Status',false,d.msg||'Cleared')).catch(function(){setMsg('azureStatus',true,'Error');});}function scanBt(){var b=document.getElementById('scanBtBtn');b.disabled=true;b.innerHTML='<span class=\"spinner\"></span>Scan';document.getElementById('btDeviceList').innerHTML='';fetch('/api/scan').then(r=>r.json()).then(d=>{b.disabled=false;b.innerHTML='Scan Bluetooth';setMsg('btStatus',false,'Found '+d.length"
");var ul=document.getElementById('btDeviceList');d.forEach(dev=>{var li=document.createElement('li');li.innerHTML='<div><div class=\"dev-name\">'+dev.name+'</div><div class=\"dev-mac\">'+dev.mac+'</div></div><button onclick=\"connectBt(\\''+dev.mac+'\\',this)\">Save</button>';ul.appendChild(li);});}).catch(e=>{b.disabled=false;b.innerHTML='Scan Bluetooth';setMsg('btStatus',true,'Scan error');});}function "
"connectBt(mac,btn){btn.disabled=true;btn.innerText='...';fetch('/api/connect',{method:'POST',body:JSON.stringify({mac:mac})}).then(r=>r.json()).then(function(){btn.innerText='Saved';setMsg('btStatus',false,'Saved — reboot to connect');}).catch(function(){btn.disabled=false;btn.innerText='Save';setMsg('btStatus',true,'Error');});}function fmtSize(n){if(n<1024)return n+' B';if(n<1048576)return (n/10"
"24).toFixed(1)+' KB';return (n/1048576).toFixed(2)+' MB';}function fetchT(url,opt,ms){ms=ms||8000;var c=new AbortController();var t=setTimeout(function(){c.abort();},ms);opt=opt||{};opt.signal=c.signal;return fetch(url,opt).finally(function(){clearTimeout(t);});}function refreshAudio(){var b=document.getElementById('audioRefreshBtn');b.disabled=true;b.innerHTML='...';fetchT('/api/audio/list').then"
"(r=>r.json()).then(d=>{b.disabled=false;b.innerHTML='Refresh';var ul=document.getElementById('audioFileList');ul.innerHTML='';if(d.error){ul.innerHTML='<li class=\"empty\">'+d.error+'</li>';document.getElementById('audioCount').innerText='—';setMsg('audioStatus',true,d.error);return;}var files=d.files||[];document.getElementById('audioCount').innerText=files.length+' file(s)';setMsg('audioStatus',fa"
"lse,files.length?'':'No files on SD');if(!files.length){ul.innerHTML='<li class=\"empty\">No audio files on SD</li>';return;}files.forEach(f=>{var li=document.createElement('li');var n=String(f.name).replace(/'/g,'');li.innerHTML='<div><div class=\"dev-name\">'+f.name+'</div><div class=\"dev-mac\">'+fmtSize(f.size)+'</div></div><button class=\"btn-del\" onclick=\"deleteAudio(\\''+n+'\\')\">Delete</button>';ul"
".appendChild(li);});}).catch(e=>{b.disabled=false;b.innerHTML='Refresh';document.getElementById('audioFileList').innerHTML='<li class=\"empty\">List timeout/failed — tap Refresh</li>';document.getElementById('audioCount').innerText='—';setMsg('audioStatus',true,'List failed');});}function deleteAudio(name){if(!confirm('Delete '+name+'?'))return;fetchT('/api/audio/delete',{method:'POST',headers:{'Con"
"tent-Type':'application/json'},body:JSON.stringify({fileName:name})}).then(r=>r.json()).then(d=>{setMsg('audioStatus',!d.ok,d.msg||(d.ok?'Deleted':'Failed'));refreshAudio();}).catch(function(){setMsg('audioStatus',true,'Error');});}function downloadAudio(){var url=document.getElementById('audioUrl').value.trim();var name=document.getElementById('audioName').value.trim();if(!url||!name){setMsg('aud"
"ioStatus',true,'Url and FileName required');return;}var b=document.getElementById('audioDlBtn');b.disabled=true;b.innerHTML='<span class=\"spinner\"></span>DL';fetchT('/api/audio/download',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({url:url,fileName:name})}).then(r=>r.json()).then(d=>{b.disabled=false;b.innerHTML='Download to SD';setMsg('audioStatus',!d.ok,d.msg||"
"(d.ok?'OK':'Failed'));if(d.ok)refreshAudio();}).catch(function(){b.disabled=false;b.innerHTML='Download to SD';setMsg('audioStatus',true,'Error');});}fetchT('/api/config').then(r=>r.json()).then(d=>{if(d.wifi_ssid)document.getElementById('wifiHidden').value=d.wifi_ssid;if(d.azure_host)document.getElementById('azureHost').value=d.azure_host;if(d.azure_devid)document.getElementById('azureDevId').val"
"ue=d.azure_devid;if(d.azure_key)document.getElementById('azureKey').value=d.azure_key;}).catch(function(){});setTimeout(refreshAudio,300);";

/* Send with WiFi priority + small chunks (SW3/BT → EAGAIN trên body lớn) */
static esp_err_t httpd_send_static(httpd_req_t *req, const char *type, const char *body)
{
    const size_t total = strlen(body);
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);

    /* Body nhỏ: gửi một lần */
    if (total <= 1536) {
        esp_err_t err = httpd_resp_send(req, body, total);
        esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "send %s failed: %s", type, esp_err_to_name(err));
        }
        return err;
    }

    /* Body lớn: chunk + retry khi EAGAIN (không delay khi OK) */
    const size_t chunk = 256;
    for (size_t off = 0; off < total; off += chunk) {
        size_t n = total - off;
        if (n > chunk) {
            n = chunk;
        }
        esp_err_t err = ESP_FAIL;
        for (int retry = 0; retry < 80; retry++) {
            err = httpd_resp_send_chunk(req, body + off, n);
            if (err == ESP_OK) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "send abort %s at %u/%u", type, (unsigned)off, (unsigned)total);
            httpd_resp_send_chunk(req, NULL, 0);
            esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
            return err;
        }
    }
    esp_err_t end = httpd_resp_send_chunk(req, NULL, 0);
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    return end;
}

/* ═══════════════════════════════════════════════════════════════
 *  HTTP HANDLERS
 * ═══════════════════════════════════════════════════════════════ */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    /* HTML nhỏ; CSS/JS tải riêng — tránh 1 gói ~11KB fail dưới BT */
    return httpd_send_static(req, "text/html", html_page);
}

static esp_err_t css_get_handler(httpd_req_t *req)
{
    return httpd_send_static(req, "text/css", css_page);
}

static esp_err_t js_get_handler(httpd_req_t *req)
{
    return httpd_send_static(req, "application/javascript", js_page);
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
        .scan_time.active.min = 50,
        .scan_time.active.max = 150,
    };
    esp_err_t scan_err = esp_wifi_scan_start(&scan_config, true);
    if (scan_err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(scan_err));
        cJSON *arr = cJSON_CreateArray();
        char *json = cJSON_PrintUnformatted(arr);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
        cJSON_Delete(arr);
        free(json);
        return ESP_OK;
    }

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

    char ssid_copy[64] = {0};
    char pass_copy[64] = {0};
    bool do_connect = false;
    if (ssid && ssid[0]) {
        strncpy(ssid_copy, ssid, sizeof(ssid_copy) - 1);
        if (pass) {
            strncpy(pass_copy, pass, sizeof(pass_copy) - 1);
        }
        save_wifi_to_nvs(ssid_copy, pass_copy);
        do_connect = true;
        ESP_LOGI(TAG, "WiFi saved: %s (reconnect after HTTP reply)", ssid_copy);
    }
    cJSON_Delete(root);

    /* Reply before disconnecting STA — otherwise browser fetch() fails with "Error" */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "{\"msg\":\"Saved WiFi config. Testing connection...\"}", HTTPD_RESP_USE_STRLEN);

    if (do_connect) {
        vTaskDelay(pdMS_TO_TICKS(300));
        wifi_connect_sta(ssid_copy, pass_copy);
    }
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

/* GET /api/audio/list */
static esp_err_t audio_list_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    if (!audio_files_sd_ready()) {
        httpd_resp_send(req, "{\"error\":\"SD card not ready\",\"files\":[]}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char *list = audio_files_list_json();
    if (!list) {
        httpd_resp_send(req, "{\"error\":\"list failed\",\"files\":[]}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    size_t len = strlen(list);
    char *wrap = malloc(len + 16);
    if (!wrap) {
        free(list);
        httpd_resp_send(req, "{\"error\":\"oom\",\"files\":[]}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    snprintf(wrap, len + 16, "{\"files\":%s}", list);
    free(list);
    httpd_resp_send(req, wrap, HTTPD_RESP_USE_STRLEN);
    free(wrap);
    return ESP_OK;
}

/* POST /api/audio/delete  {fileName} */
static esp_err_t audio_delete_post_handler(httpd_req_t *req)
{
    char buf[128] = {0};
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(buf);
    const char *name = NULL;
    if (root) {
        cJSON *n = cJSON_GetObjectItem(root, "fileName");
        if (n && cJSON_IsString(n)) {
            name = n->valuestring;
        }
    }
    esp_err_t e = name ? audio_files_delete(name) : ESP_ERR_INVALID_ARG;
    if (root) {
        cJSON_Delete(root);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    if (e == ESP_OK) {
        httpd_resp_send(req, "{\"ok\":true,\"msg\":\"Deleted\"}", HTTPD_RESP_USE_STRLEN);
    } else if (e == ESP_ERR_INVALID_STATE) {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"SD card not ready\"}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"Delete failed\"}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

/* POST /api/audio/download {url,fileName} */
static esp_err_t audio_download_post_handler(httpd_req_t *req)
{
    char buf[512] = {0};
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(buf);
    const char *url = NULL;
    const char *name = NULL;
    if (root) {
        cJSON *u = cJSON_GetObjectItem(root, "url");
        cJSON *n = cJSON_GetObjectItem(root, "fileName");
        if (u && cJSON_IsString(u)) {
            url = u->valuestring;
        }
        if (n && cJSON_IsString(n)) {
            name = n->valuestring;
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    bool ok = (url && name && audio_files_request_download(url, name));
    if (root) {
        cJSON_Delete(root);
    }

    if (ok) {
        httpd_resp_send(req, "{\"ok\":true,\"msg\":\"Download queued\"}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"Queue failed (SD missing or invalid args)\"}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

/* GET /api/scan — Bluetooth device scan (existing) */
static esp_err_t bt_scan_get_handler(httpd_req_t *req)
{
    /* Initialize Bluetooth subsystem dynamically when user scans in Config Mode */
    user_bluetooth_init();

    start_bt_device_scan();
    vTaskDelay(pdMS_TO_TICKS(7000));

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
    config.max_open_sockets = 4;
    config.max_uri_handlers = 24;
    config.stack_size = 8192;
    config.send_wait_timeout = 10;
    config.recv_wait_timeout = 10;

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK)
    {
        ESP_LOGI(TAG, "Registering URI handlers");

        /* Root page + static assets (smaller chunks → fewer EAGAIN) */
        httpd_register_uri_handler(server, &uri_root);
        httpd_uri_t css_uri = { .uri = "/style.css", .method = HTTP_GET, .handler = css_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &css_uri);
        httpd_uri_t js_uri = { .uri = "/app.js", .method = HTTP_GET, .handler = js_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &js_uri);

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

        /* Audio files on SD */
        httpd_uri_t alist_uri = { .uri = "/api/audio/list", .method = HTTP_GET, .handler = audio_list_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &alist_uri);
        httpd_uri_t adel_uri = { .uri = "/api/audio/delete", .method = HTTP_POST, .handler = audio_delete_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &adel_uri);
        httpd_uri_t adl_uri = { .uri = "/api/audio/download", .method = HTTP_POST, .handler = audio_download_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &adl_uri);

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
    /* SoftAP: 192.168.4.1 (Config). STA: IP LAN khi WiFi đã connected. */
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        ESP_LOGI("HTTP SERVER", "Starting HTTP server — http://" IPSTR, IP2STR(&ip_info.ip));
    } else {
        ESP_LOGI("HTTP SERVER", "Starting HTTP server (AP: http://192.168.4.1)");
    }
    start_webserver();

    while(1)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
