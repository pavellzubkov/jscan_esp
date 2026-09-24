#pragma once
#include "AppEvents.h"
#include "AppConfig.h"
#include "EventManager.h"
#include "esp_event.h"

struct AppContext {
    esp_event_loop_handle_t event_loop = nullptr;
    EventManager events;
    AppConfig config;   // пишет только ConfigStore

    AppContext() = default;
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