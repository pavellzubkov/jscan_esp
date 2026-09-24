#pragma once
#include "esp_http_server.h"

// Регистрирует глобальный GET /* для раздачи статики из SPIFFS
// (index.html для '/', gzip .gz fallback, Access-Control-Allow-Origin: *).
esp_err_t reg_static_handler(httpd_handle_t server);