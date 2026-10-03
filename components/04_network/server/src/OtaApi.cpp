#include "OtaApi.hpp"
#include "OtaService.hpp"
#include "HttpCommon.hpp"
#include "esp_log.h"
#include "cJSON.h"

static const char* TAG = "OtaApi";

// OtaService живёт весь процесс (член ServerModule). httpd передаёт его через
// user_ctx каждого URI, поэтому глобальная переменная не нужна.
static esp_err_t ota_get_ctx(httpd_req_t* req, OtaService** out) {
    *out = static_cast<OtaService*>(req->user_ctx);
    if (!*out) {
        http::setStatus(req, http::status::kInternalServerError);
        return httpd_resp_sendstr(req, R"({"error":"ota unavailable"})");
    }
    return ESP_OK;
}

// GET /api/ota/status — текущее состояние OTA
static esp_err_t ota_status_get_handler(httpd_req_t* req) {
    OtaService* ota = nullptr;
    esp_err_t err = ota_get_ctx(req, &ota);
    if (err != ESP_OK) return err;

    http::setJsonHeaders(req);
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_sendstr(req, R"({"error":"oom"})");
    }
    ota->fillStatusJson(root);

    char* json = cJSON_PrintUnformatted(root);
    esp_err_t res = httpd_resp_sendstr(req, json ? json : "{}");
    cJSON_free(json);
    cJSON_Delete(root);
    return res;
}

// POST /api/ota/storage — приём и запись образа LittleFS (storage)
static esp_err_t ota_storage_post_handler(httpd_req_t* req) {
    OtaService* ota = nullptr;
    esp_err_t err = ota_get_ctx(req, &ota);
    if (err != ESP_OK) return err;
    return ota->handleStorageUpload(req);
}

// POST /api/ota/app — приём и запись основной прошивки (OTA)
static esp_err_t ota_app_post_handler(httpd_req_t* req) {
    OtaService* ota = nullptr;
    esp_err_t err = ota_get_ctx(req, &ota);
    if (err != ESP_OK) return err;
    return ota->handleAppUpload(req);
}

// Единая регистрация одного URI с логом ошибки (порядок регистрации важен:
// конкретные URI — до wildcard /* статики, см. ServerModule::begin).
static esp_err_t reg_one(httpd_handle_t server, const httpd_uri_t& uri) {
    esp_err_t ret = httpd_register_uri_handler(server, &uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register %s: %s", uri.uri,
                 esp_err_to_name(ret));
    }
    return ret;
}

// Таблица OTA-эндпоинтов: добавление URI — одна строка (вместо копипасты
// httpd_uri_t). Порядок регистрации сохраняет текущий (сверху вниз) и
// обязан оставаться ДО wildcard-хендлера статики (см. ServerModule::begin).
struct UriDesc {
    const char* uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t*);
};

static constexpr UriDesc kOtaUris[] = {
    {"/api/ota/status", HTTP_GET,  ota_status_get_handler},
    {"/api/ota/storage", HTTP_POST, ota_storage_post_handler},
    {"/api/ota/app",     HTTP_POST, ota_app_post_handler},
};

esp_err_t OtaApi::reg(httpd_handle_t server, OtaService* ota) {
    if (!server || !ota) {
        ESP_LOGE(TAG, "invalid args (server=%p ota=%p)", (void*)server, (void*)ota);
        return ESP_ERR_INVALID_ARG;
    }

    for (const UriDesc& d : kOtaUris) {
        httpd_uri_t uri = {
            .uri = d.uri,
            .method = d.method,
            .handler = d.handler,
            .user_ctx = ota,
            .is_websocket = false,
            .handle_ws_control_frames = false,
            .supported_subprotocol = nullptr,
        };
        esp_err_t ret = reg_one(server, uri);
        if (ret != ESP_OK) return ret;
    }

    ESP_LOGI(TAG, "OTA endpoints registered");
    return ESP_OK;
}