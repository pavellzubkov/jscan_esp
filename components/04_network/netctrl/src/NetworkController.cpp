#include "NetworkController.hpp"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"

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

    // Системные сетевые предпосылки (нужны Wi-Fi и netif). Здесь, а не в main —
    // сетевой модуль владеет всем сетевым стеком.
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Сначала радио, потом httpd (клиент сможет подключиться сразу к работающему).
    err = wifi_.begin();
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
