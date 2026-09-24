#include "NetworkController.hpp"
#include "esp_log.h"

static const char* TAG = "NetCtrl";

NetworkController::NetworkController(AppContext* ctx)
    : ctx_(ctx), wifi_(ctx), server_(ctx) {}

NetworkController::~NetworkController() {
    stop();
}

esp_err_t NetworkController::begin() {
    if (!ctx_ || !ctx_->event_loop) {
        ESP_LOGE(TAG, "Invalid context or event loop");
        return ESP_ERR_INVALID_ARG;
    }

    // Сначала радио, потом httpd (клиент сможет подключиться сразу к работающему).
    esp_err_t err = wifi_.begin();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi softAP start failed (%s) — degraded mode, network down",
                 esp_err_to_name(err));
        return err;
    }

    err = server_.begin();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP/WS server start failed (%s) — degraded mode, network down",
                 esp_err_to_name(err));
        wifi_.stop();
        return err;
    }

    ESP_LOGI(TAG, "network up (softAP + HTTP/WS)");
    return ESP_OK;
}

void NetworkController::stop() {
    // Сначала httpd (чтобы не дёргать esp_wifi под живым сервером), потом радио.
    server_.stop();
    wifi_.stop();
}
