#include <utility>
#include "AppContext.h"
#include "BootManager.h"
#include "ConfigStore.h"
#include "CommModule.h"
#include "NetworkController.hpp"
#include "J1939System.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Единая точка создания модуля: new -> begin -> delete при ошибке.
template <typename Module, typename... Args>
static esp_err_t makeModule(AppContext* ctx, Args&&... args) {
    Module* m = new Module(ctx, std::forward<Args>(args)...);
    esp_err_t err = m->begin();
    if (err != ESP_OK) { delete m; }
    return err;
}

#define REGISTER_MODULE(name, Type, priority, critical, ...) \
    boot.add({name, [](AppContext* c) { return makeModule<Type>(c, ##__VA_ARGS__); }, priority, critical})

extern "C" void app_main() {
    ESP_ERROR_CHECK(esp_event_loop_create_default());   // системные события (WiFi)
    ESP_ERROR_CHECK(esp_netif_init());                   // сетевой интерфейс

    AppContext ctx;
    ESP_ERROR_CHECK(ctx.initEventLoop());

    BootManager boot;

    // Приоритет сортируется по возрастанию: 0 = первый, 255 = последний.
    // critical = true останавливает загрузку при ошибке; false — degraded mode.

    // config — критичен: конфиг (AP, снапшоты) нужен всем; монтирует LittleFS /config.
    REGISTER_MODULE("config",  ConfigStore,         10, true, "/config", "config");
    // comm — протокол/кадры/команды; подписывается на WS-события.
    REGISTER_MODULE("comm",    CommunicationModule, 20, false);
    // netctrl — владеет wifi + server (HTTP/WS/static). Некритичен: без сети
    // J1939-скан продолжает работать (degraded mode).
    REGISTER_MODULE("netctrl", NetworkController,   40, false);
    // j1939 — TWAI-приём, TP, снапшоты.
    REGISTER_MODULE("j1939",   J1939System,         70, false);

#undef REGISTER_MODULE

    esp_err_t overall = boot.startAll(&ctx);
    if (overall != ESP_OK) {
        ESP_LOGW("MAIN", "Some modules failed — degraded mode");
    }

    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}