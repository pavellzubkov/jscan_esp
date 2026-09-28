#include "WifiApModule.hpp"
#include "simple_dns_server.hpp"
#include "HardwareConfig.h"
#include "LogicUtils.h"
#include "esp_log.h"
#include <cstdio>
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

    // Пересчитываем число подключённых клиентов и публикуем runtime-поля.
    wifi_sta_list_t staList = {};
    esp_err_t err = esp_wifi_ap_get_sta_list(&staList);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get STA list: %s", esp_err_to_name(err));
    }

    {
        AppDataLock dataLock(ctx_);   // запись из системного loop — нужен мьютекс
        ctx_->fields.writeFieldScalar(wifiClients_UID, staList.num);
        ctx_->fields.writeFieldScalar(wifiApMode_UID, true);
    }
    sendField(ctx_, wifiClients_UID);
    sendField(ctx_, wifiApMode_UID);

    // Статус для логов CommunicationModule (WIFI_STATUS).
    wifi_status_event_t status = {};
    status.is_ap_mode = true;
    status.is_connected = (staList.num > 0);
    status.num_clients = staList.num;
    status.rssi = 0;
    status.disconnect_reason = 0;
    ctx_->events.post(APP_EVENTS_BASE, app_event_id_t::WIFI_STATUS, status);
}

// Парсинг "a.b.c.d" в uint32 в network byte order (формат esp_ip4_addr_t.addr).
// При невалидной строке остаются дефолтные 10.10.10.10.
uint32_t WifiApModule::parseIp(const char* ip) {
    int a = 10, b = 10, c = 10, d = 10;
    if (ip) sscanf(ip, "%d.%d.%d.%d", &a, &b, &c, &d);
    return (static_cast<uint32_t>(a) & 0xFF) |
           ((static_cast<uint32_t>(b) & 0xFF) << 8) |
           ((static_cast<uint32_t>(c) & 0xFF) << 16) |
           ((static_cast<uint32_t>(d) & 0xFF) << 24);
}

// Пересоздание DNS-сервера (captive portal) на актуальный IP AP.
void WifiApModule::restartDns(uint32_t ip) {
    if (dns_) {
        dns_->stop();
        delete dns_;
        dns_ = nullptr;
    }
    dns_ = new DnsServer(ip);
    if (dns_ && !dns_->start()) {
        ESP_LOGW(TAG, "Failed to start DNS server");
        delete dns_;
        dns_ = nullptr;
    } else {
        ESP_LOGI(TAG, "Captive portal DNS server started (ip=0x%08lX)",
                 (unsigned long)ip);
    }
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

    // Live-apply: переприменять конфиг AP при изменении WIFI-полей.
    ctx_->events.subscribe(APP_EVENTS_BASE, app_event_id_t::CONFIG_CHANGED,
                           &WifiApModule::onConfigChanged, this);

    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = applyConfig();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "applyConfig failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    started_ = true;
    ESP_LOGI(TAG, "softAP started: ssid=%s channel=%u maxSta=%u",
             ctx_->adata.apSsid, ctx_->adata.apChannel,
             ctx_->adata.maxStaConn);
    return ESP_OK;
}

esp_err_t WifiApModule::applyConfig() {
    // Статический IP точки доступа из конфигурации (apIp).
    const uint32_t ip = parseIp(ctx_->adata.apIp);
    esp_netif_ip_info_t ipInfo = {};
    ipInfo.ip.addr      = ip;
    ipInfo.gw.addr      = ip;
    ipInfo.netmask.addr = Hw::kApNetmask;
    esp_netif_dhcps_stop(netif_);
    esp_netif_set_ip_info(netif_, &ipInfo);
    esp_netif_dhcps_start(netif_);

    // Конфигурация AP из ctx->adata (пишет ConfigStore).
    wifi_config_t wifiConfig = {};
    strlcpy(reinterpret_cast<char*>(wifiConfig.ap.ssid),
            ctx_->adata.apSsid, sizeof wifiConfig.ap.ssid);
    strlcpy(reinterpret_cast<char*>(wifiConfig.ap.password),
            ctx_->adata.apPassword, sizeof wifiConfig.ap.password);
    wifiConfig.ap.channel = ctx_->adata.apChannel;
    wifiConfig.ap.max_connection = ctx_->adata.maxStaConn;
    wifiConfig.ap.authmode =
        (ctx_->adata.apPassword.data[0] != '\0') ? WIFI_AUTH_WPA2_PSK
                                                 : WIFI_AUTH_OPEN;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_AP, &wifiConfig);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    restartDns(ip);
    return ESP_OK;
}

void WifiApModule::onConfigChanged(const field_change_event_t* evt) {
    if (!evt) return;

    switch (evt->uid) {
    case apSsid_UID:
    case apPassword_UID:
    case apChannel_UID:
    case maxStaConn_UID:
    case apIp_UID:
        break;
    default:
        return;   // не наше поле
    }

    ESP_LOGI(TAG, "AP config changed (uid=0x%04X), re-applying",
             evt->uid);

    if (applyConfig() != ESP_OK) {
        ESP_LOGE(TAG, "applyConfig failed on live change");
        return;
    }

    // Кратковременный обрыв: клиенты отвалятся и переподключатся.
    // (esp_wifi_restart() в IDF 6.1 нет — используем stop+start.)
    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(ret));
        return;
    }
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
    }
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