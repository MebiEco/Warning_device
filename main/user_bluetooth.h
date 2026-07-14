#ifndef USER_BLUETOOTH_H
#define USER_BLUETOOTH_H

#include "esp_bt_defs.h"
#include "esp_gap_bt_api.h"
#include <stdint.h>
#include <stdbool.h>

#define MAX_DISCOVERED_DEVICES 30

typedef struct {
    esp_bd_addr_t bda;
    char bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
    bool is_used;
} bt_device_info_t;

extern bt_device_info_t discovered_devices[MAX_DISCOVERED_DEVICES];
extern int discovered_devices_count;
extern bool is_manual_connect_requested;
extern int s_bt_retry_count;

void user_bluetooth_init(void);
void user_bluetooth_deinit(void);
bool user_bluetooth_is_connected(void);
bool user_bluetooth_is_link_ready(void);
bool user_bluetooth_wait_for_connect(uint32_t timeout_ms);
void start_bt_device_scan(void);
void connect_to_bt_device(uint8_t *mac_addr, const char *name);

#endif // USER_BLUETOOTH_H
