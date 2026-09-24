#pragma once
#include "AppContext.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include <cstdint>

class DnsServer;

// SoftAP-точка доступа. Конфиг (ssid/pass/channel/maxStaConn) из ctx->config.
// Публикует WIFI_STATUS на событиях AP_STACONNECTED/AP_STADISCONNECTED.
// Владеет жизненным циклом DNS-сервера captive portal (весь DNS → IP AP).
class WifiApModule {
public:
    explicit WifiApModule(AppContext* ctx);
    ~WifiApModule();

    esp_err_t begin();   // esp_wifi_init + create_default_wifi_ap + start + DNS
    void      stop();

private:
    AppContext* ctx_;
    esp_netif_t* netif_ = nullptr;
    DnsServer* dns_ = nullptr;
    bool started_ = false;

    static void eventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
    void onEvent(esp_event_base_t base, int32_t id, void* data);
};