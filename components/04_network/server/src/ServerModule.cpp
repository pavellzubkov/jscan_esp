#include "ServerModule.hpp"
#include "StaticHandler.hpp"
#include "WsHandler.hpp"
#include "OtaApi.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char* TAG = "ServerModule";

ServerModule::ServerModule(AppContext* ctx)
    : ctx_(ctx), fs_("/littlefs", "storage", false) {}

ServerModule::~ServerModule() {
    stop();
}

esp_err_t ServerModule::begin() {
    if (server_) {
        ESP_LOGW(TAG, "HTTP server already running");
        return ESP_OK;
    }

    // Монтируем ФС (идемпотентно).
    esp_err_t ret = fs_.mount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // OTA-хендлеры выполняют длительные flash-операции (стирание/запись) прямо
    // в задаче httpd. Увеличенный стек страхует от переполнения в этих путях.
    config.stack_size = 16384;
    config.max_uri_handlers = 32;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.global_user_ctx = ctx_;

    // OtaService делит с ServerModule одну FS (storage): OTA размонтирует её
    // на время записи образа.
    ota_.init(ctx_, &fs_);

    ESP_LOGI(TAG, "Starting HTTP server...");
    ret = httpd_start(&server_, &config);
    if (ret != ESP_OK) {
        server_ = nullptr;
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "HTTP server started, free heap: %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    // Порядок важен: статик-хендлер регистрирует wildcard `/*`, который через
    // httpd_uri_match_wildcard матчит любой URI. Если зарегистрировать его первым,
    // httpd_register_uri_handler будет считать /api/ota/* и /ws уже занятыми
    // (ESP_ERR_HTTPD_HANDLER_EXISTS), а поиск хендлера пойдёт по порядку
    // регистрации. Поэтому сначала конкретные URI, wildcard — последним.
    OtaApi::reg(server_, &ota_);

    ws_ = new WsHandler(ctx_);
    esp_err_t ws_ret = ws_->reg(server_);
    if (ws_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WS handler: %s",
                 esp_err_to_name(ws_ret));
    }

    reg_static_handler(server_);

    ESP_LOGI(TAG, "ServerModule ready");
    return ESP_OK;
}

void ServerModule::stop() {
    if (ws_) {
        ws_->unreg();
        delete ws_;
        ws_ = nullptr;
    }
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}