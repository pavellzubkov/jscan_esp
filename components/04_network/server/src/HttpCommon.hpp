#pragma once
#include <esp_http_server.h>

// Общие HTTP-хелперы для хендлеров сервера: заголовки CORS/JSON и
// строковые коды статусов (единая точка правды).
namespace http {

namespace status {
inline constexpr const char *kOk                  = "200 OK";
inline constexpr const char *kAccepted            = "202 Accepted";
inline constexpr const char *kFound               = "302 Found";
inline constexpr const char *kNotModified         = "304 Not Modified";
inline constexpr const char *kBadRequest          = "400 Bad Request";
inline constexpr const char *kNotFound            = "404 Not Found";
inline constexpr const char *kMethodNotAllowed    = "405 Method Not Allowed";
inline constexpr const char *kConflict            = "409 Conflict";
inline constexpr const char *kInternalServerError = "500 Internal Server Error";
inline constexpr const char *kServiceUnavailable  = "503 Service Unavailable";
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