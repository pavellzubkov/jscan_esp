#pragma once
#include "AppEvents.h"
#include "AppData.h"
#include "EventManager.h"
#include "FieldRegistry.h"
#include "esp_event.h"
#include <mutex>

// Контекст приложения: контейнер общего состояния и сервисов.
struct AppContext {
    AppData adata;
    std::recursive_mutex adataMutex;  // защита составных/много-полевых операций над adata
    esp_event_loop_handle_t event_loop = nullptr;
    EventManager events;

    // Реестр полей с контролем владения (обёртка над adata).
    FieldRegistry fields;

    AppContext() : fields(adata, adataMutex) {}
    ~AppContext() {
        events.shutdown();            // снять подписки ДО удаления loop
        if (event_loop) esp_event_loop_delete(event_loop);
    }
    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;

    esp_err_t initEventLoop() {
        esp_event_loop_args_t args = {
            .queue_size = 256,
            .task_name = "app_event_loop",
            .task_priority = 5,
            .task_stack_size = 4096,
            .task_core_id = 0};  // ядро 0 безопасно для ESP32 и ESP32-S3
        esp_err_t err = esp_event_loop_create(&args, &event_loop);
        if (err != ESP_OK) return err;
        events.setEventLoop(event_loop);
        return ESP_OK;
    }
};

// RAII guard: блокирует доступ к общему AppData на время составной/много-полевой операции.
// Определён после struct AppContext, чтобы обращаться к adataMutex без неполного типа.
class AppDataLock {
public:
    explicit AppDataLock(AppContext* ctx) : mutex_(ctx ? &ctx->adataMutex : nullptr) {
        if (mutex_) mutex_->lock();
    }
    ~AppDataLock() {
        if (mutex_) mutex_->unlock();
    }
    AppDataLock(const AppDataLock&) = delete;
    AppDataLock& operator=(const AppDataLock&) = delete;

private:
    std::recursive_mutex* mutex_;
};