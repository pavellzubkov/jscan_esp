#include "SystemStatusModule.h"

#include "LogicUtils.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

namespace {
const char* TAG = "sys_status";

// Стек задачи маленький: публикуются только три скалярных поля.
constexpr uint16_t kTaskStackSize = 4096;
constexpr uint8_t  kTaskPriority  = 1;
constexpr uint8_t  kTaskCore      = 0;   // ядро 0 безопасно для ESP32 и ESP32-S3

constexpr uint32_t kPeriodMs = 1000;
}

SystemStatusModule::SystemStatusModule(AppContext* ctx)
    : ctx_(ctx)
{
}

SystemStatusModule::~SystemStatusModule()
{
    if (task_)
    {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    if (ctx_)
        ctx_->events.unsubscribe(this);
}

esp_err_t SystemStatusModule::begin()
{
    if (xTaskCreatePinnedToCore(taskWrapper, "sys_status", kTaskStackSize, this,
                                kTaskPriority, &task_, kTaskCore) != pdPASS)
    {
        ESP_LOGE(TAG, "task create failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

void SystemStatusModule::taskWrapper(void* p)
{
    static_cast<SystemStatusModule*>(p)->taskLoop();
}

void SystemStatusModule::taskLoop()
{
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(kPeriodMs));

        AppDataLock dataLock(ctx_);   // атомарная публикация блока SYSTEM-полей

        const uint32_t uptimeMs = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        const uint32_t heapFree = static_cast<uint32_t>(esp_get_free_heap_size());

        ctx_->fields.writeFieldScalar(fwVersion_UID, FixedString("1.0.0"));
        ctx_->fields.writeFieldScalar(uptimeMs_UID, uptimeMs);
        ctx_->fields.writeFieldScalar(heapFree_UID, heapFree);

        sendField(ctx_, fwVersion_UID);
        sendField(ctx_, uptimeMs_UID);
        sendField(ctx_, heapFree_UID);
    }
}