#ifndef USER_WIFI_H
#define USER_WIFI_H

#include "esp_err.h"
#include <stdbool.h>

void wifi_init_config_mode(void);
void wifi_init_normal_mode(void);

#endif // USER_WIFI_H
