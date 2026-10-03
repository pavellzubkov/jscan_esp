#pragma once
#include "AppContext.hpp"
#include "LittleFsService.hpp"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdint>
#include <string>

// Хранилище конфигурации на реестре полей (AppData/FieldRegistry).
// Монтирует LittleFS (если не смонтирован), читает /config/config.json при старте,
// применяя поля по именам (getMetaByName, неизвестные ключи игнорируются),
// сохраняет с дебаунсом при изменении (подписка на CONFIG_CHANGED),
// обрабатывает FACTORY_RESET (сброс к дефолтам + unlink + esp_restart).
class ConfigStore {
public:
    ConfigStore(AppContext* ctx, const char* basePath = "/config",
                const char* partitionLabel = "config");
    ~ConfigStore();

    esp_err_t begin();   // монтирует FS, грузит конфиг, создаёт таск автосейва

private:
    static constexpr const char* kConfigPath = "/config.json";   // в /config

    AppContext* ctx_;
    std::string basePath_;
    std::string partitionLabel_;
    LittleFsService fs_;

    void loadFromFs();       // читает JSON → applyFieldsJson (нет файла → дефолты + dirty)
    void saveToFs();         // buildFieldsJson → JSON → атомарная запись

    // --- JSON-слой поверх реестра полей (cJSON доступен через espressif__cjson) ---
    cJSON* buildFieldsJson();        // только isConfig-поля по meta.name
    bool applyFieldsJson(cJSON* root, bool trustedRestore,
                         bool* hadUnknown = nullptr);

    void onConfigChanged(const field_change_event_t* evt);  // → dirty_ = true
    esp_err_t reset();       // factory reset: дефолты + unlink + esp_restart

    static void autoSaveWrapper(void* p);
    void autoSaveLoop();     // раз в ~1 с: если dirty → saveToFs()
    // Graceful stop автосейва (паттерн DnsServer::stop): stop_ ->
    // autoSaveLoop выходит (после vTaskDelay, ДО новой saveToFs) -> give
    // doneSem_ -> vTaskDelete(nullptr); ждём с таймаутом, fallback —
    // принудительный vTaskDelete. Иначе задачу можно убить посреди записи
    // в flash (open/write/rename). Вызывается из dtor и reset() (до unlink).
    void stopAutoSave();
    std::atomic<bool> dirty_ = false;
    std::atomic<bool> stop_{false};
    SemaphoreHandle_t doneSem_ = nullptr;
    TaskHandle_t task_ = nullptr;
};