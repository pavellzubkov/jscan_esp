#ifndef ___FS_SPIFFS_SD_H__
#define ___FS_SPIFFS_SD_H__

#include <app_common.h>
#include "esp_netif.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "esp_vfs_semihost.h"
// #include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "sdmmc_cmd.h"


esp_err_t init_fs(AppState *adr);

#endif // ___FS_SPIFFS_SD_H__