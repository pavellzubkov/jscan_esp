#include "WifiApModule.hpp"
#include "simple_dns_server.hpp"
#include "HardwareConfig.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "WifiApModule";

WifiApModule::WifiApModule(AppContext* ctx) : ctx_(ctx) {}

WifiApModule::~WifiApModule() {
    stop();
}

void WifiApModule::eventHandler(void* arg, esp_event_base_t base, int32_t id,
                                void* data) {
    auto* self = static_cast<WifiApModule*>(arg);
    if (self) self->onEvent(base, id, data);
}

void WifiApModule::onEvent(esp_event_base_t base, int32_t id, void* /*data*/) {
    if (base != WIFI_EVENT) return;
    if (id != WIFI_EVENT_AP_STACONNECTED && id != WIFI_EVENT_AP_STADISCONNECTED)
        return;

    // Пересчитываем число подключённых клиентов и публикуем статус.
    wifi_sta_list_t staList = {};
    esp_err_t err = esp_wifi_ap_get_sta_list(&staList);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get STA list: %s", esp_err_to_name(err));
    }

    wifi_status_event_t status = {};
    status.is_ap_mode = true;
    status.is_connected = (staList.num > 0);
    status.num_clients = staList.num;
    status.rssi = 0;
    status.disconnect_reason = 0;
    ctx_->events.post(APP_EVENTS_BASE, app_event_id_t::WIFI_STATUS, status);
}

esp_err_t WifiApModule::begin() {
    if (started_) return ESP_OK;

    // esp_netif_init() гарантирует NetworkController::begin() перед вызовом.
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);  // отключить power save

    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &eventHandler, this, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register wifi event handler: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    netif_ = esp_netif_create_default_wifi_ap();
    if (!netif_) {
        ESP_LOGE(TAG, "Failed to create default wifi AP netif");
        return ESP_FAIL;
    }

    // Статический IP точки доступа (из HardwareConfig).
    esp_netif_ip_info_t ipInfo = {};
    ipInfo.ip.addr      = Hw::kApIp;
    ipInfo.gw.addr      = Hw::kApGateway;
    ipInfo.netmask.addr = Hw::kApNetmask;
    esp_netif_dhcps_stop(netif_);
    esp_netif_set_ip_info(netif_, &ipInfo);
    esp_netif_dhcps_start(netif_);

    // Конфигурация AP из ctx->config (пишет ConfigStore).
    wifi_config_t wifiConfig = {};
    strlcpy(reinterpret_cast<char*>(wifiConfig.ap.ssid),
            ctx_->config.apSsid, sizeof wifiConfig.ap.ssid);
    strlcpy(reinterpret_cast<char*>(wifiConfig.ap.password),
            ctx_->config.apPassword, sizeof wifiConfig.ap.password);
    wifiConfig.ap.channel = ctx_->config.apChannel;
    wifiConfig.ap.max_connection = ctx_->config.maxStaConn;
    wifiConfig.ap.authmode =
        (ctx_->config.apPassword[0] != '\0') ? WIFI_AUTH_WPA2_PSK
                                             : WIFI_AUTH_OPEN;

    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_wifi_set_config(WIFI_IF_AP, &wifiConfig);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    started_ = true;
    ESP_LOGI(TAG, "softAP started: ssid=%s channel=%u maxSta=%u",
             ctx_->config.apSsid, ctx_->config.apChannel,
             ctx_->config.maxStaConn);

    // Запуск DNS-сервера (captive portal): все DNS-запросы клиентов AP
    // резолвятся на IP точки доступа, где их перехватит HTTP-редирект.
    if (dns_) {
        dns_->stop();
        delete dns_;
    }
    dns_ = new DnsServer(Hw::kApIp);
    if (dns_ && !dns_->start()) {
        ESP_LOGW(TAG, "Failed to start DNS server");
        delete dns_;
        dns_ = nullptr;
    } else {
        ESP_LOGI(TAG, "Captive portal DNS server started on %s",
                 "10.10.10.10");
    }
    return ESP_OK;
}

void WifiApModule::stop() {
    if (!started_) return;

    // Остановка DNS-сервера (AP/captive portal)
    if (dns_) {
        dns_->stop();
        delete dns_;
        dns_ = nullptr;
    }

    esp_wifi_stop();
    esp_wifi_deinit();
    if (netif_) {
        esp_netif_destroy_default_wifi(netif_);
        netif_ = nullptr;
    }
    started_ = false;
    ESP_LOGI(TAG, "softAP stopped");
}