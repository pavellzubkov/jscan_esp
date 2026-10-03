#pragma once
#include "AppContext.hpp"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include <cstdint>
#include <memory>

class DnsServer;

// SoftAP-точка доступа. Конфиг (ssid/pass/channel/maxStaConn/apIp) из ctx->adata.
// Live-apply: пачка изменений WIFI-полей дебаунсится одноразовым esp_timer
// (500 мс) → событие WIFI_REAPPLY → одно применение конфига (wifi_config_t +
// статический IP) через esp_wifi_stop()/esp_wifi_start() вместо N рестартов —
// кратковременный обрыв, клиенты переподключатся. Смена IP дополнительно
// пересоздаёт DNS-сервер.
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
    // unique_ptr: владение DNS-сервером (captive portal) с авто-удалением;
    // порядок остановки задаёт stop()/restartDns (сначала dns_->stop()).
    // ~WifiApModule объявлен в hpp и определён в .cpp, где DnsServer
    // полный тип — удаление объекта через unique_ptr безопасно.
    std::unique_ptr<DnsServer> dns_;
    bool started_ = false;     // esp_wifi_start() выполнен (AP поднят)
    bool wifiInited_ = false;  // esp_wifi_init() выполнен → нужен esp_wifi_deinit

    // Дебаунс live-apply: одноразовый таймер, по истечении которого в шину
    // постится WIFI_REAPPLY (сам таймер работает в esp_timer task и не трогает
    // wifi API напрямую). nullptr = fallback на немедленное применение.
    esp_timer_handle_t reapplyTimer_ = nullptr;
    // Обработчик WIFI_EVENT (esp_netif/wifi) — для корректного unregister в stop().
    esp_event_handler_instance_t wifiEvtInst_ = nullptr;

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
    // Отложенное применение (колбэк WIFI_REAPPLY): тело live-apply из
    // onConfigChanged. Вызывается из event-loop задачи — wifi API здесь безопасен.
    void onWifiReapply();
    // Дебаунс: перезапустить таймер; при его отсутствии — fallback, применить сейчас.
    void scheduleReapply();
    static void reapplyTimerCb(void* arg);
};