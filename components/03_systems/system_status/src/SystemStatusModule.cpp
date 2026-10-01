#include "SystemStatusModule.h"

#include "HardwareConfig.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

namespace {
const char* TAG = "sys_status";
}

SystemStatusModule::SystemStatusModule(AppContext* ctx)
    : ctx_(ctx)
{
}

SystemStatusModule::~SystemStatusModule()
{
    if (ctx_)
        ctx_->events.unsubscribe(this);
}

esp_err_t SystemStatusModule::begin()
{
    if (!ctx_)
        return ESP_FAIL;

    // Непрерывные SYSTEM-поля считаются в момент чтения (REQUEST / push-on-
    // connect) — периодическая задача не нужна.
    ctx_->fields.setDynamicReader(&SystemStatusModule::readDynamicSystemField);

    // fwVersion — статично, пишется один раз при старте.
    const FieldWriteStatus st =
        ctx_->fields.writeFieldString(fwVersion_UID, Hw::kFwVersion);
    if (st != FieldWriteStatus::OK) {
        ESP_LOGE(TAG, "fwVersion write failed: %d", static_cast<int>(st));
    }

    ESP_LOGI(TAG, "started (SYSTEM fields computed on read)");
    return ESP_OK;
}

// Ленивый подсчёт uptimeMs/heapFree при чтении. Не трогает AppData — сразу
// сериализует uint32 (raw LE) в out, поэтому мьютекс не требуется.
// out==nullptr — запрос размера (семантика FieldRegistry::readField).
bool SystemStatusModule::readDynamicSystemField(uint16_t uid, uint8_t* out,
                                                size_t out_cap,
                                                size_t* out_len)
{
    constexpr size_t kSize = sizeof(uint32_t);
    if (out_len)
        *out_len = kSize;
    if (!out)
        return true;
    if (out_cap < kSize)
        return false;

    uint32_t value;
    if (uid == uptimeMs_UID)
        value = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    else if (uid == heapFree_UID)
        value = static_cast<uint32_t>(esp_get_free_heap_size());
    else
        return false;   // не наше динамическое поле

    out[0] = static_cast<uint8_t>(value & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
    return true;
}