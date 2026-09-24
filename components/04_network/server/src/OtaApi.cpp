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

void OtaApi::reg(httpd_handle_t server, OtaService* ota) {
    if (!server || !ota) {
        ESP_LOGE(TAG, "invalid args (server=%p ota=%p)", (void*)server, (void*)ota);
        return;
    }

    httpd_uri_t ota_status_uri = {
        .uri = "/api/ota/status",
        .method = HTTP_GET,
        .handler = ota_status_get_handler,
        .user_ctx = ota,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server, &ota_status_uri);

    httpd_uri_t ota_storage_uri = {
        .uri = "/api/ota/storage",
        .method = HTTP_POST,
        .handler = ota_storage_post_handler,
        .user_ctx = ota,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server, &ota_storage_uri);

    httpd_uri_t ota_app_uri = {
        .uri = "/api/ota/app",
        .method = HTTP_POST,
        .handler = ota_app_post_handler,
        .user_ctx = ota,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server, &ota_app_uri);

    ESP_LOGI(TAG, "OTA endpoints registered");
}