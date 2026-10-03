#pragma once
#include "AppContext.hpp"

// Системный статус (SYSTEM-домен): владелец полей fwVersion/uptimeMs/heapFree.
// Периодических задач нет:
//   * fwVersion — статично, пишется один раз при старте;
//   * uptimeMs/heapFree — непрерывные значения, вычисляются «на лету» при
//     чтении (FieldDynamicReader): PARAM_REQUEST / push-on-connect всегда
//     возвращают свежие данные без PUSH-спама раз в секунду.
class SystemStatusModule {
public:
    explicit SystemStatusModule(AppContext* ctx);
    ~SystemStatusModule();

    esp_err_t begin();

private:
    AppContext* ctx_;

    // Ленивый подсчёт непрерывных SYSTEM-полей при чтении.
    static bool readDynamicSystemField(uint16_t uid, uint8_t* out,
                                       size_t out_cap, size_t* out_len);
};