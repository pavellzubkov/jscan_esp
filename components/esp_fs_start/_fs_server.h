#ifndef ___FS_SERVER_H__
#define ___FS_SERVER_H__
#include <app_common.h>

esp_err_t start_rest_server(const char *base_path,AppState *adr);
esp_err_t start_http(AppState *adr);

#endif // ___FS_SERVER_H__