#pragma once
#include "AppContext.h"
#include "AppEvents.h"
#include <cstring>

// Отправка одного поля по коммуникационному модулю (broadcast)
inline void sendField(AppContext* ctx, uint16_t uid)
{
    communication_send_event_t evt = { uid, -1 };
    postEvent<app_event_id_t::COMMUNICATION_SEND>(ctx->events, evt);
}

// Запись runtime-поля с diff+push: под AppDataLock сравнивает новое значение
// со старым, сохраняет, и если поле реально изменилось — пушит его клиентам
// (PARAM_PUSH broadcast). Возвращает true при изменении.
// Аналог «агрегатора» из TEMP_PID, но без центрального модуля: владелец
// домена пишет своё поле сам, а эта функция избавляет от безусловного
// sendField и лишнего PUSH-спама.
// Для строковых полей (FixedString) здесь writeFieldScalar не годится —
// используйте FieldRegistry::writeFieldString + sendField отдельно.
template <typename T>
bool updateField(AppContext* ctx, uint16_t uid, const T& value)
{
    if (!ctx)
        return false;

    uint8_t oldBuf[kAppMaxFieldSize + 8];
    size_t oldLen = 0;
    bool changed = false;

    {
        AppDataLock lock(ctx);   // чтение+запись — одна атомарная операция
        const bool hadOld = ctx->fields.readField(uid, oldBuf, sizeof(oldBuf),
                                                  &oldLen);
        ctx->fields.writeFieldScalar(uid, value);

        uint8_t newBuf[kAppMaxFieldSize + 8];
        size_t newLen = 0;
        ctx->fields.readField(uid, newBuf, sizeof(newBuf), &newLen);
        changed = !hadOld || oldLen != newLen ||
                  memcmp(oldBuf, newBuf, oldLen) != 0;
    }

    if (changed)
        sendField(ctx, uid);
    return changed;
}

// Опубликовать изменение поля в общей шине приложения.
// Позволяет доменным функциям общаться друг с другом через тот же
// механизм, что и запись по протоколу (app_event_id_t::CONFIG_CHANGED).
inline void postFieldChanged(AppContext* ctx, uint16_t uid)
{
    field_change_event_t evt = { uid };
    postEvent<app_event_id_t::CONFIG_CHANGED>(ctx->events, evt);
}