#pragma once

#include "esp_http_server.h"

class OtaService;

// Регистрация HTTP-эндпоинтов OTA: /api/ota/status (GET),
// /api/ota/storage (POST), /api/ota/app (POST). Возвращает первую ошибку
// httpd_register_uri_handler (Caller откатывает begin()).
namespace OtaApi {
esp_err_t reg(httpd_handle_t server, OtaService* ota);
}