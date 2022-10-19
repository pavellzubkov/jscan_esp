
#ifndef __ESP_FS_START_H__
#define __ESP_FS_START_H__

#include <app_common.h>

namespace FSService
{
    esp_err_t RunSPIFFS(AppState *adr);
    esp_err_t RunHTTP(AppState *adr);
    esp_err_t RunREST(AppState *adr);
    esp_err_t InitConfig(AppState *adr);
} // namespace FSService



#endif // __ESP_FS_START_H__