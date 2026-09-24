#pragma once

#include "esp_http_server.h"

class OtaService;

// Регистрация HTTP-эндпоинтов OTA: /api/ota/status (GET),
// /api/ota/storage (POST), /api/ota/app (POST).
namespace OtaApi {
void reg(httpd_handle_t server, OtaService* ota);
}