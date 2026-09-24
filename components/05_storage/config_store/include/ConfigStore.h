#pragma once
#include "AppContext.h"
#include "SpiffsService.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdint>

// Хранилище конфигурации. Единственный писатель ctx->config (load/defaults).
// Монтирует SPIFFS (если не смонтирован), читает /spiffs/config.json при старте,
// сохраняет с дебаунсом при изменении, публикует CONFIG_CHANGED.
class ConfigStore {
public:
    explicit ConfigStore(AppContext* ctx);
    ~ConfigStore();

    esp_err_t begin();   // монтирует FS, грузит конфиг, создаёт таск автосейва

    // Загрузить значение поля в ctx->config и сохранить (для будущих команд
    // протокола). Сейчас только чтение.
    void setAndSave(const AppConfig& next);   // не используется в этом этапе — задел

private:
    static constexpr const char* kConfigPath = "/config.json";   // в /spiffs

    AppContext* ctx_;
    SpiffsService fs_;

    void loadFromFs();       // читает JSON → ctx->config (нет файла → дефолты + save)
    void saveToFs();         // ctx->config → JSON → fs_

    static void autoSaveWrapper(void* p);
    void autoSaveLoop();     // раз в ~1 с: если dirty → saveToFs()
    bool dirty_ = false;
    TaskHandle_t task_ = nullptr;
};