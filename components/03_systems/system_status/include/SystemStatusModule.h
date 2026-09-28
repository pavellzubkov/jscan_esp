#pragma once
#include "AppContext.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Системный статус: публикует runtime-поля SYSTEM-домена
// (fwVersion/uptimeMs/heapFree) раз в секунду.
class SystemStatusModule {
public:
    explicit SystemStatusModule(AppContext* ctx);
    ~SystemStatusModule();

    esp_err_t begin();

private:
    AppContext* ctx_;
    TaskHandle_t task_ = nullptr;

    static void taskWrapper(void* p);
    void taskLoop();
};