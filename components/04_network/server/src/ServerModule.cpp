#include "ServerModule.hpp"
#include "StaticHandler.hpp"
#include "WsHandler.hpp"
#include "OtaApi.hpp"
#include "HttpCommon.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char* TAG = "ServerModule";

ServerModule::ServerModule(AppContext* ctx)
    : ctx_(ctx), fs_(http::kStaticMountPath, "storage", false) {}

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
    // Ошибка любой регистрации — откат через stop() и возврат ошибки:
    // молчаливый ESP_OK оставил бы систему с нерабочим /ws или статикой.
    ret = OtaApi::reg(server_, &ota_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register OTA endpoints: %s",
                 esp_err_to_name(ret));
        stop();
        return ret;
    }

    ws_ = std::make_unique<WsHandler>(ctx_);
    ret = ws_->reg(server_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WS handler: %s",
                 esp_err_to_name(ret));
        stop();
        return ret;
    }

    // reg_static_handler владение отдаёт через out-параметр → берём в
    // unique_ptr (при ошибке хендлер сам delete'ит контекст, out = nullptr).
    static_ctx_t* staticCtxRaw = nullptr;
    ret = reg_static_handler(server_, &staticCtxRaw);
    staticCtx_.reset(staticCtxRaw);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register static handler: %s",
                 esp_err_to_name(ret));
        stop();
        return ret;
    }

    ESP_LOGI(TAG, "ServerModule ready");
    return ESP_OK;
}

void ServerModule::stop() {
    // Сначала полностью останавливаем httpd (завершает все задачи/сокеты),
    // затем удаляем контексты хендлеров. Иначе httpd-задачи могут вызывать
    // хендлеры с освобождённым user_ctx (UAF).
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
    if (ws_) {
        ws_->unreg();
        ws_.reset();      // unique_ptr удаляет WsHandler
    }
    // Контекст статики (~4 КБ со scratch) — строго после httpd_stop:
    // wildcard-хендлер /* больше не вызывается.
    staticCtx_.reset();
}