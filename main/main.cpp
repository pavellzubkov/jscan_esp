#include "AppContext.h"
#include "BootManager.h"
#include "ConfigStore.h"
#include "CommModule.h"
#include "NetworkController.hpp"
#include "J1939System.h"
#include "SystemStatusModule.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main() {
    AppContext ctx;
    ESP_ERROR_CHECK(ctx.initEventLoop());

    BootManager boot;

    // Приоритет сортируется по возрастанию: 0 = первый, 255 = последний.
    // critical = true останавливает загрузку при ошибке; false — degraded mode.

    // config — критичен: конфиг (AP, снапшоты) нужен всем; монтирует LittleFS /config,
    // инициализирует NVS (нужен Wi-Fi).
    REGISTER_MODULE(boot, "config",  ConfigStore,         10, true,  "/config", "config");
    // comm — протокол/кадры/команды; подписывается на WS-события.
    REGISTER_MODULE(boot, "comm",    CommunicationModule, 20, false);
    // netctrl — владеет wifi + server (HTTP/WS/static). Некритичен: без сети
    // J1939-скан продолжает работать (degraded mode).
    REGISTER_MODULE(boot, "netctrl", NetworkController,   40, false);
    // sysinfo — системный статус (SYSTEM-домен): fwVersion пишется один раз;
    // uptimeMs/heapFree вычисляются на лету при чтении (без периодических задач).
    REGISTER_MODULE(boot, "sysinfo", SystemStatusModule,  50, false);
    // j1939 — TWAI-приём, TP, снапшоты.
    REGISTER_MODULE(boot, "j1939",   J1939System,         70, false);

    esp_err_t overall = boot.startAll(&ctx);
    if (overall != ESP_OK) {
        ESP_LOGW("MAIN", "Some modules failed — degraded mode");
    }

    // Подтверждаем OTA-образ только после успешного старта критичных
    // модулей + короткого health-check: если система падает в первые
    // секунды, mark не выполнится и bootloader откатит прошлое.
    if (boot.isReady("config")) {
        vTaskDelay(pdMS_TO_TICKS(5000));   // health-check: живём 5 с
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI("MAIN", "OTA image validated");
    } else {
        ESP_LOGE("MAIN", "critical module failed — OTA image NOT validated, rollback on next boot");
    }

    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}