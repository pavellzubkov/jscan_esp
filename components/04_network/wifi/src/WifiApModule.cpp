#include "WifiApModule.hpp"
#include "simple_dns_server.hpp"
#include "HardwareConfig.h"
#include "LogicUtils.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>

static const char* TAG = "WifiApModule";

// Дебаунс live-apply: окно, за которое пачка изменений WIFI-полей сводится
// к одному stop/start AP (500 мс).
static constexpr int64_t kReapplyDebounceUs = 500000;

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
    postEvent<app_event_id_t::WIFI_STATUS>(ctx_->events, status);
}

// Парсинг "a.b.c.d" в uint32 в network byte order (формат esp_ip4_addr_t.addr).
// Строгий разбор: ровно 4 октета, каждый 0..255, без мусора в хвосте —
// поле apIp уже валидируется CFG_IP-валидатором в FieldRegistry, здесь это
// вторая линия защиты. При любой ошибке — дефолт Hw::kApIp + лог.
uint32_t WifiApModule::parseIp(const char* ip) {
    int a = 0, b = 0, c = 0, d = 0;
    char tail = '\0';
    // %c ловит всё после 4-го октета: лишний символ → 5 conversions → отказ.
    if (ip && sscanf(ip, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) == 4 &&
        a >= 0 && a <= 255 && b >= 0 && b <= 255 &&
        c >= 0 && c <= 255 && d >= 0 && d <= 255) {
        return (static_cast<uint32_t>(a) & 0xFF) |
               ((static_cast<uint32_t>(b) & 0xFF) << 8) |
               ((static_cast<uint32_t>(c) & 0xFF) << 16) |
               ((static_cast<uint32_t>(d) & 0xFF) << 24);
    }
    ESP_LOGW(TAG, "invalid apIp '%s', using default", ip ? ip : "(null)");
    return Hw::kApIp;
}

// Пересоздание DNS-сервера (captive portal) на актуальный IP AP.
void WifiApModule::restartDns(uint32_t ip) {
    if (dns_) {
        dns_->stop();   // остановить задачу ДО удаления объекта (UAF)
        dns_.reset();
    }
    dns_ = std::make_unique<DnsServer>(ip);
    if (dns_ && !dns_->start()) {
        ESP_LOGW(TAG, "Failed to start DNS server");
        dns_.reset();
    } else {
        ESP_LOGI(TAG, "Captive portal DNS server started (ip=0x%08lX)",
                 (unsigned long)ip);
    }
}

esp_err_t WifiApModule::begin() {
    if (started_) return ESP_OK;

    // Откат при ЛЮБОЙ ошибке — через stop(). Поэтому stop() обязан быть
    // ресурсо-ориентированным (проверяет хэндлы/флаги, а не started_):
    // частично созданные netif_/dns_/wifi-init освобождаются всегда,
    // а не только когда begin() дошёл до started_ = true (иначе утечка).
    // Примечание: makeModule удалит объект при ошибке begin(), но полагаться
    // на это как на ЕДИНСТВЕННЫЙ откат нельзя — stop() здесь делает явный откат.

    // esp_netif_init() гарантирует NetworkController::begin() перед вызовом.
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;   // ресурсов ещё нет — откат не нужен
    }
    wifiInited_ = true;
    esp_wifi_set_ps(WIFI_PS_NONE);  // отключить power save

    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &eventHandler, this, &wifiEvtInst_);
    if (ret != ESP_OK) {
        wifiEvtInst_ = nullptr;
        ESP_LOGE(TAG, "Failed to register wifi event handler: %s",
                 esp_err_to_name(ret));
        stop();
        return ret;
    }

    netif_ = esp_netif_create_default_wifi_ap();
    if (!netif_) {
        ESP_LOGE(TAG, "Failed to create default wifi AP netif");
        stop();
        return ESP_FAIL;
    }

    // Дебаунс live-apply: одноразовый таймер сводит пачку изменений WIFI-полей
    // к одному stop/start. Ошибка создания — не фатально, включается fallback
    // (немедленное применение в scheduleReapply).
    esp_timer_create_args_t timerArgs = {};
    timerArgs.callback = &WifiApModule::reapplyTimerCb;
    timerArgs.arg = this;
    timerArgs.dispatch_method = ESP_TIMER_TASK;
    timerArgs.name = "wifi_reapply";
    timerArgs.skip_unhandled_events = true;
    ret = esp_timer_create(&timerArgs, &reapplyTimer_);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_timer_create failed: %s (immediate reapply)",
                 esp_err_to_name(ret));
        reapplyTimer_ = nullptr;
    }

    // Live-apply: переприменять конфиг AP при изменении WIFI-полей (с дебаунсом)
    // и по отложенному событию WIFI_REAPPLY.
    if (!subscribeEvent<app_event_id_t::CONFIG_CHANGED>(
            ctx_->events, &WifiApModule::onConfigChanged, this)) {
        ESP_LOGE(TAG, "subscribe(CONFIG_CHANGED) failed");
        stop();
        return ESP_FAIL;
    }
    if (!subscribeEvent<app_event_id_t::WIFI_REAPPLY>(
            ctx_->events, &WifiApModule::onWifiReapply, this)) {
        ESP_LOGE(TAG, "subscribe(WIFI_REAPPLY) failed");
        stop();
        return ESP_FAIL;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        stop();
        return ret;
    }
    ret = applyConfig();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "applyConfig failed: %s", esp_err_to_name(ret));
        stop();
        return ret;
    }
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        stop();
        return ret;
    }

    started_ = true;

    FixedString apSsid;
    uint8_t apChannel = 6, maxStaConn = 2;
    ctx_->fields.getByName("apSsid", apSsid);
    ctx_->fields.getByName("apChannel", apChannel);
    ctx_->fields.getByName("maxStaConn", maxStaConn);
    ESP_LOGI(TAG, "softAP started: ssid=%s channel=%u maxSta=%u",
             apSsid.data, apChannel, maxStaConn);
    return ESP_OK;
}

esp_err_t WifiApModule::applyConfig() {
    // Конфиг AP читаем через реестр полей (под локом) — единая точка доступа.
    FixedString apIp, apSsid, apPassword;
    uint8_t apChannel = 6, maxStaConn = 2;
    ctx_->fields.getByName("apIp", apIp);
    ctx_->fields.getByName("apSsid", apSsid);
    ctx_->fields.getByName("apPassword", apPassword);
    ctx_->fields.getByName("apChannel", apChannel);
    ctx_->fields.getByName("maxStaConn", maxStaConn);

    // Статический IP точки доступа из конфигурации (apIp).
    const uint32_t ip = parseIp(apIp.data);
    esp_netif_ip_info_t ipInfo = {};
    ipInfo.ip.addr      = ip;
    ipInfo.gw.addr      = ip;
    ipInfo.netmask.addr = Hw::kApNetmask;
    // Коды проверяем и логируем: это конфиг AP (не фатально — продолжаем),
    // но молчаливый отказ dhcps/set_ip_info маскировал бы проблему.
    esp_err_t netifErr = esp_netif_dhcps_stop(netif_);
    if (netifErr != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_dhcps_stop failed: %s", esp_err_to_name(netifErr));
    }
    netifErr = esp_netif_set_ip_info(netif_, &ipInfo);
    if (netifErr != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_set_ip_info failed: %s", esp_err_to_name(netifErr));
    }
    netifErr = esp_netif_dhcps_start(netif_);
    if (netifErr != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_dhcps_start failed: %s", esp_err_to_name(netifErr));
    }

    // Конфигурация AP из AppData (пишет ConfigStore).
    wifi_config_t wifiConfig = {};
    strlcpy(reinterpret_cast<char*>(wifiConfig.ap.ssid),
            apSsid.data, sizeof wifiConfig.ap.ssid);
    strlcpy(reinterpret_cast<char*>(wifiConfig.ap.password),
            apPassword.data, sizeof wifiConfig.ap.password);
    wifiConfig.ap.channel = apChannel;
    wifiConfig.ap.max_connection = maxStaConn;
    wifiConfig.ap.authmode =
        (apPassword.data[0] != '\0') ? WIFI_AUTH_WPA2_PSK
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

    ESP_LOGI(TAG, "AP config changed (uid=0x%04X), scheduling re-apply",
             evt->uid);
    scheduleReapply();   // пачка изменений → одно применение после дебаунса
}

// Дебаунс: перезапустить одноразовый таймер (окно 500 мс). Если таймер не
// создан — fallback: применить немедленно (поведение до дебаунса).
void WifiApModule::scheduleReapply() {
    if (reapplyTimer_) {
        esp_err_t ret = esp_timer_restart(reapplyTimer_, kReapplyDebounceUs);
        if (ret == ESP_OK) return;
        ESP_LOGW(TAG, "esp_timer_restart failed: %s (immediate reapply)",
                 esp_err_to_name(ret));
    }
    onWifiReapply();
}

// Колбэк таймера: работает в esp_timer task — wifi/netif API отсюда звать
// нельзя, только thread-safe post в event-loop.
void WifiApModule::reapplyTimerCb(void* arg) {
    auto* self = static_cast<WifiApModule*>(arg);
    if (self && self->ctx_) {
        postEvent<app_event_id_t::WIFI_REAPPLY>(self->ctx_->events);
    }
}

// Отложенное применение конфига AP (из event-loop задачи — wifi API безопасен).
void WifiApModule::onWifiReapply() {
    if (!started_ || !netif_) return;   // AP остановлен — применять нечего

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
    // Ресурсо-ориентированная очистка: каждый шаг проверяет СВОЁ состояние
    // (хэндл/флаг), а не общий started_. Поэтому stop() безопасен на любом
    // этапе begin() — он же служит откатом error-path'ов в begin() и dtor.
    // Идемпотентен: повторные вызовы (dtor после ручного stop) — no-op.

    // Отписка от шины событий — первой: обработчики не должны дёргать
    // модуль после остановки/удаления.
    ctx_->events.unsubscribe(this);

    // Дебаунс-таймер — до остановки wifi (порядок: таймер, затем wifi).
    if (reapplyTimer_) {
        esp_timer_stop(reapplyTimer_);
        esp_timer_delete(reapplyTimer_);
        reapplyTimer_ = nullptr;
    }

    if (wifiEvtInst_) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifiEvtInst_);
        wifiEvtInst_ = nullptr;
    }

    // Остановка DNS-сервера (AP/captive portal) — независимо от started_:
    // dns_ создаётся в applyConfig() ещё ДО esp_wifi_start.
    if (dns_) {
        dns_->stop();   // остановить задачу ДО удаления объекта (UAF)
        dns_.reset();   // unique_ptr удаляет объект; dtor вызовет stop() ещё
                        // раз — он идемпотентен
    }

    const bool wasStarted = started_;

    if (started_) {
        esp_wifi_stop();
        started_ = false;
    }

    // esp_wifi_deinit — строго при удавшемся esp_wifi_init (флаг wifiInited_):
    // без флага деинициализировали бы нетронутый драйвер на ранних error-path.
    if (wifiInited_) {
        esp_wifi_deinit();
        wifiInited_ = false;
    }

    if (netif_) {
        esp_netif_destroy_default_wifi(netif_);
        netif_ = nullptr;
    }

    if (wasStarted)
        ESP_LOGI(TAG, "softAP stopped");
}