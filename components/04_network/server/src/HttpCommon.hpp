#pragma once
#include <esp_http_server.h>

// Общие HTTP-хелперы для хендлеров сервера: заголовки CORS/JSON и
// строковые коды статусов (единая точка правды).
namespace http {

// Базовый путь раздачи статики (партиция storage, LittleFS) — единая точка
// правды: его монтирует LittleFsService в ServerModule и его же копирует
// static_ctx_t при регистрации wildcard-хендлера.
inline constexpr const char *kStaticMountPath = "/littlefs";

// Лимит URI-хендлеров httpd (httpd_config_t.max_uri_handlers): покрывает
// OTA (3) + WS + статику + запас на будущие роуты; захардкожен не был —
// теперь единая константа вместо литерала 32 в ServerModule::begin.
inline constexpr int kMaxUriHandlers = 32;

namespace status {
inline constexpr const char *kOk                  = "200 OK";
inline constexpr const char *kFound               = "302 Found";
inline constexpr const char *kBadRequest          = "400 Bad Request";
inline constexpr const char *kConflict            = "409 Conflict";
inline constexpr const char *kInternalServerError = "500 Internal Server Error";
} // namespace status

// Разрешить кросс-доменные запросы (web-UI обращается с другого origin).
inline void setCors(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

inline void setStatus(httpd_req_t *req, const char *status) {
    httpd_resp_set_status(req, status);
}

// CORS + Content-Type: application/json — типовые заголовки JSON-API.
inline void setJsonHeaders(httpd_req_t *req) {
    setCors(req);
    httpd_resp_set_type(req, "application/json");
}

} // namespace http