#ifndef USER_WIFI_H
#define USER_WIFI_H

#include "esp_err.h"
#include <stdbool.h>

void wifi_init_config_mode(void);
void wifi_init_normal_mode(void);
void wifi_connect_sta(const char *ssid, const char *pass);

#endif // USER_WIFI_H
