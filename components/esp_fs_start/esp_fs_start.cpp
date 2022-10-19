#include <esp_fs_start.h>
#include <_fs_server.h>
#include <_fs_spiffs_sd.h>
#include "nvs_flash.h"

namespace FSService
{

    esp_err_t RunSPIFFS(AppState *adr)
    {
        ESP_ERROR_CHECK(nvs_flash_init());
        /* Start the SPIFFS */
        ESP_ERROR_CHECK(init_fs(adr));
        return ESP_OK;
    }

    esp_err_t RunHTTP(AppState *adr)
    {
        /* Start the file server */
        ESP_ERROR_CHECK(start_http(adr));
        return ESP_OK;
    }

    esp_err_t RunREST(AppState *adr)
    {
        /* Start the file server */
        ESP_ERROR_CHECK(start_rest_server(WEB_MOUNT_POINT, adr));
        return ESP_OK;
    }


}