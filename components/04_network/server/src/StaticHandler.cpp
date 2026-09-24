#include "StaticHandler.hpp"
#include "esp_log.h"
#include "esp_vfs.h"
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

static const char* TAG = "StaticHandler";

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)

// Буфер для потоковой передачи файла — в heap (не на стеке httpd-задачи).
// ~4 КБ на чанк вместо загрузки всего файла в RAM.
static constexpr size_t kScratchBufSize = 4096;

#define CHECK_FILE_EXTENSION(filename, ext) \
    (strlen(filename) >= strlen(ext) &&      \
     strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

// Контекст хендлера: базовый путь ФС + scratch-буфер для чтения.
struct static_ctx_t {
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[kScratchBufSize];
};

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

static esp_err_t static_get_handler(httpd_req_t* req) {
    auto* ctx = static_cast<static_ctx_t*>(req->user_ctx);
    if (!ctx) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Bad server context");
        return ESP_FAIL;
    }

    if (is_uri_safe(req) != ESP_OK) return ESP_FAIL;

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
    while ((n = read(fd, ctx->scratch, kScratchBufSize)) > 0) {
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

esp_err_t reg_static_handler(httpd_handle_t server) {
    auto* ctx = new static_ctx_t();
    strlcpy(ctx->base_path, "/littlefs", sizeof ctx->base_path);

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
    return ESP_OK;
}