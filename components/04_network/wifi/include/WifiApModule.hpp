#pragma once
#include "AppContext.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include <cstdint>

class DnsServer;

// SoftAP-точка доступа. Конфиг (ssid/pass/channel/maxStaConn/apIp) из ctx->adata.
// Live-apply: подписка на CONFIG_CHANGED переприменяет конфиг AP (wifi_config_t +
// статический IP) через esp_wifi_stop()/esp_wifi_start() — кратковременный обрыв,
// клиенты переподключатся. Смена IP дополнительно пересоздаёт DNS-сервер.
// Публикует runtime-поля wifiClients/wifiApMode + WIFI_STATUS на событиях
// AP_STACONNECTED/AP_STADISCONNECTED.
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

    // Собрать и применить конфиг AP из adata: парсинг apIp → IP netif (dhcps),
    // wifi_config_t (ssid/pass/channel/maxStaConn) + DNS-сервер на актуальный IP.
    esp_err_t applyConfig();
    // Пересоздать DNS-сервер captive portal на новый IP (при смене apIp).
    void restartDns(uint32_t ip);
    // Парсинг "a.b.c.d" в uint32 в network byte order (формат esp_ip4_addr_t.addr).
    static uint32_t parseIp(const char* ip);

    static void eventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
    void onEvent(esp_event_base_t base, int32_t id, void* data);
    void onConfigChanged(const field_change_event_t* evt);
};