#pragma once
#include "AppContext.h"
#include "LittleFsService.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdint>
#include <string>

// Хранилище конфигурации. Единственный писатель ctx->config (load/defaults).
// Монтирует LittleFS (если не смонтирован), читает /config/config.json при старте,
// сохраняет с дебаунсом при изменении, публикует CONFIG_CHANGED.
class ConfigStore {
public:
    ConfigStore(AppContext* ctx, const char* basePath = "/config",
                const char* partitionLabel = "config");
    ~ConfigStore();

    esp_err_t begin();   // монтирует FS, грузит конфиг, создаёт таск автосейва

    // Загрузить значение поля в ctx->config и сохранить (для будущих команд
    // протокола). Сейчас только чтение.
    void setAndSave(const AppConfig& next);   // не используется в этом этапе — задел

private:
    static constexpr const char* kConfigPath = "/config.json";   // в /config

    AppContext* ctx_;
    std::string basePath_;
    std::string partitionLabel_;
    LittleFsService fs_;

    void loadFromFs();       // читает JSON → ctx->config (нет файла → дефолты + save)
    void saveToFs();         // ctx->config → JSON → fs_

    static void autoSaveWrapper(void* p);
    void autoSaveLoop();     // раз в ~1 с: если dirty → saveToFs()
    bool dirty_ = false;
    TaskHandle_t task_ = nullptr;
};