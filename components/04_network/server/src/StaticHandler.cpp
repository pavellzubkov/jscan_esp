#include "StaticHandler.hpp"
#include "HttpCommon.hpp"
#include "AppContext.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

static const char* TAG = "StaticHandler";

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)

#define CHECK_FILE_EXTENSION(filename, ext) \
    (strlen(filename) >= strlen(ext) &&      \
     strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

// Проверка безопасности URI: запрет обхода директорий (..) и //.
static esp_err_t is_uri_safe(httpd_req_t* req) {
    const char* uri = req->uri;
    if (!uri || strlen(uri) == 0) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Access denied");
        return ESP_FAIL;
    }
    if (strstr(uri, "..") || strstr(uri, "//")) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Access denied");
        return ESP_FAIL;
    }
    const char* basename = strrchr(uri, '/');
    if (basename && basename[1] == '.') {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Access denied");
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Content-Type по расширению файла (без учёта .gz).
static void set_content_type_from_file(httpd_req_t* req, const char* filepath) {
    const char* type = "text/plain";

    char name[64];
    strlcpy(name, filepath, sizeof name);
    size_t len = strlen(name);
    if (len > 3 && strcmp(name + len - 3, ".gz") == 0) {
        name[len - 3] = '\0';
    }

    if (CHECK_FILE_EXTENSION(name, ".html"))
        type = "text/html";
    else if (CHECK_FILE_EXTENSION(name, ".js"))
        type = "application/javascript";
    else if (CHECK_FILE_EXTENSION(name, ".css"))
        type = "text/css";
    else if (CHECK_FILE_EXTENSION(name, ".png"))
        type = "image/png";
    else if (CHECK_FILE_EXTENSION(name, ".ico"))
        type = "image/x-icon";
    else if (CHECK_FILE_EXTENSION(name, ".svg"))
        type = "image/svg+xml";
    else if (CHECK_FILE_EXTENSION(name, ".json"))
        type = "application/json";
    else if (CHECK_FILE_EXTENSION(name, ".jpg") ||
             CHECK_FILE_EXTENSION(name, ".jpeg"))
        type = "image/jpeg";
    else if (CHECK_FILE_EXTENSION(name, ".woff"))
        type = "font/woff";
    else if (CHECK_FILE_EXTENSION(name, ".woff2"))
        type = "font/woff2";

    httpd_resp_set_type(req, type);
}

// Собрать полный путь к файлу: base_path + uri (+ index.html для '/').
static void build_filepath(static_ctx_t* ctx, const char* uri,
                           char* filepath, size_t filepath_size) {
    strlcpy(filepath, ctx->base_path, filepath_size);
    size_t base_len = strlen(filepath);
    strlcat(filepath, uri, filepath_size);

    // Строка запроса и якорь не входят в имя файла.
    char* quest = strchr(filepath + base_len, '?');
    if (quest) *quest = '\0';
    char* hash = strchr(filepath + base_len, '#');
    if (hash) *hash = '\0';

    // Запрос корня/директории → index.html.
    size_t len = strlen(filepath);
    if (len > 0 && filepath[len - 1] == '/') {
        strlcat(filepath, "index.html", filepath_size);
    }
}

// Совпадение Host с доменом: точное равенство ИЛИ суффикс ".<domain>" в
// конце. strstr запрещён — он даёт ложные совпадения (evilapple.com
// совпал бы с apple.com).
static bool host_matches_domain(const char* host, const char* domain) {
    size_t hlen = strlen(host);
    size_t dlen = strlen(domain);
    if (hlen == dlen) return strcmp(host, domain) == 0;
    // Ровно ".domain" на конце: разделитель — точка перед доменом.
    return hlen > dlen + 1 && host[hlen - dlen - 1] == '.' &&
           strcmp(host + (hlen - dlen), domain) == 0;
}

// Ультра-агрессивный captive-перехват. Работает безусловно: сеть jscan всегда
// работает как softAP (нет STA-режима), поэтому редирект на портал всегда уместен.
static esp_err_t is_captive(httpd_req_t* req) {
    // IP портала — из конфигурации apIp (может меняться по протоколу), а не
    // хардкод. global_user_ctx задаёт ServerModule::begin (AppContext).
    AppContext* app =
        static_cast<AppContext*>(httpd_get_global_user_ctx(req->handle));
    FixedString apIpStr;
    if (!app || !app->fields.getByName("apIp", apIpStr)) {
        ESP_LOGW(TAG, "apIp unavailable, captive disabled");
        return ESP_FAIL;
    }

    char ap_ip[16];
    strlcpy(ap_ip, apIpStr.data, sizeof(ap_ip));

    char portal_root[48];
    char portal_captive[64];
    snprintf(portal_root, sizeof(portal_root), "http://%s", ap_ip);
    snprintf(portal_captive, sizeof(portal_captive), "http://%s/#/captive", ap_ip);

    char host_buffer[100] = {0};
    const char* uri = req->uri;

    // 1. Специфичные URI — ВСЕГДА редирект на портал!
    if (strcmp(uri, "/generate_204") == 0 ||
        strcmp(uri, "/gen_204") == 0 ||
        strcmp(uri, "/ncsi.txt") == 0 ||
        strcmp(uri, "/connecttest.txt") == 0 ||
        strcmp(uri, "/success.txt") == 0 ||
        strcmp(uri, "/hotspot-detect.html") == 0) {
        ESP_LOGI(TAG, "Captive detected: %s -> REDIRECT", uri);
        http::setStatus(req, http::status::kFound);
        httpd_resp_set_hdr(req, "Location", portal_captive);
        http::setCors(req);
        httpd_resp_send(req, "Redirect to captive portal", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // 2. Получаем Host header
    esp_err_t host_result =
        httpd_req_get_hdr_value_str(req, "Host", host_buffer, sizeof(host_buffer));

    // 3. Редирект для известных captive-доменов
    if (host_result == ESP_OK && host_buffer[0] != '\0') {
        // Отрезаем :port из Host ДО всех сравнений (браузеры шлюзуют порт:
        // "10.10.10.10:80" иначе не совпало бы с ap_ip -> лишний редирект ->
        // петля). IPv4 — нас интересует первый ':'; IPv6 не наш случай.
        if (char* colon = strchr(host_buffer, ':')) *colon = '\0';

        const char* captive_domains[] = {
            "msftconnecttest.com",
            "captive.apple.com",
            "connectivity-check.ubuntu.com",
            "clients3.google.com",
            "connectivitycheck.android.com",
            "android.clients.google.com",
            "gstatic.com",
            "apple.com"
        };

        for (const char* domain : captive_domains) {
            if (host_matches_domain(host_buffer, domain)) {
                ESP_LOGI(TAG, "Captive domain redirect: %s", host_buffer);
                http::setStatus(req, http::status::kFound);
                httpd_resp_set_hdr(req, "Location", portal_captive);
                http::setCors(req);
                httpd_resp_send(req, "Redirect to captive portal", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }
        }

        // 4. Для всех остальных запросов с Host, НЕ равным нашему IP.
        // После отрезания порта Host "10.10.10.10:80" == ap_ip -> БЕЗ
        // редиректа (happy-path: обращение к порталу по IP с портом).
        if (strcmp(host_buffer, ap_ip) != 0) {
            ESP_LOGI(TAG, "Foreign host redirect: %s", host_buffer);
            http::setStatus(req, http::status::kFound);
            httpd_resp_set_hdr(req, "Location", portal_root);
            http::setCors(req);
            httpd_resp_send(req, "Redirect to captive portal", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    }

    return ESP_FAIL;
}

static esp_err_t static_get_handler(httpd_req_t* req) {
    auto* ctx = static_cast<static_ctx_t*>(req->user_ctx);
    if (!ctx) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Bad server context");
        return ESP_FAIL;
    }

    if (is_uri_safe(req) != ESP_OK) return ESP_FAIL;

    // Captive-перехват — до раздачи файлов (редирект на портал).
    if (is_captive(req) == ESP_OK) return ESP_OK;

    char filepath[FILE_PATH_MAX];
    build_filepath(ctx, req->uri, filepath, sizeof filepath);

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    // Пробуем открыть файл; при неудаче — gzip-вариант (.gz).
    int fd = open(filepath, O_RDONLY, 0);
    bool gz = false;
    if (fd == -1) {
        strlcat(filepath, ".gz", sizeof filepath);
        fd = open(filepath, O_RDONLY, 0);
        if (fd == -1) {
            ESP_LOGW(TAG, "File not found: %s", req->uri);
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
            return ESP_FAIL;
        }
        gz = true;
    }

    set_content_type_from_file(req, filepath);
    if (gz) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    // Потоковая передача чанками.
    esp_err_t ret = ESP_OK;
    ssize_t n;
    while ((n = read(fd, ctx->scratch, static_ctx_t::kScratchSize)) > 0) {
        if (httpd_resp_send_chunk(req, ctx->scratch,
                                  static_cast<size_t>(n)) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send chunk for %s", filepath);
            ret = ESP_FAIL;
            break;
        }
    }
    close(fd);

    if (ret != ESP_OK) {
        httpd_resp_sendstr_chunk(req, nullptr);
        return ret;
    }
    if (n < 0) {
        ESP_LOGE(TAG, "Failed to read file: %s", filepath);
        httpd_resp_sendstr_chunk(req, nullptr);
        return ESP_FAIL;
    }

    httpd_resp_send_chunk(req, nullptr, 0);   // конец ответа
    return ESP_OK;
}

esp_err_t reg_static_handler(httpd_handle_t server, static_ctx_t** out_ctx) {
    if (!out_ctx) return ESP_ERR_INVALID_ARG;
    *out_ctx = nullptr;
    if (!server) return ESP_ERR_INVALID_ARG;

    auto* ctx = new static_ctx_t();
    strlcpy(ctx->base_path, http::kStaticMountPath, sizeof ctx->base_path);

    httpd_uri_t uri = {};
    uri.uri = "/*";
    uri.method = HTTP_GET;
    uri.handler = static_get_handler;
    uri.user_ctx = ctx;
    uri.is_websocket = false;
    uri.handle_ws_control_frames = false;

    esp_err_t ret = httpd_register_uri_handler(server, &uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register static handler: %s",
                 esp_err_to_name(ret));
        delete ctx;
        return ret;
    }
    ESP_LOGI(TAG, "Static handler registered (base: %s)", ctx->base_path);
    *out_ctx = ctx;   // владение переходит к вызывающему (ServerModule)
    return ESP_OK;
}