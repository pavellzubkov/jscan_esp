#pragma once
#include "AppContext.hpp"
#include "AppEvents.hpp"
#include "esp_log.h"

// Отправка одного поля по коммуникационному модулю (broadcast).
// Возврат post намеренно не проверяется: дроп события (очередь event-loop
// полна) уже логируется внутри EventManager со счётчиком droppedEvents().
inline void sendField(AppContext* ctx, uint16_t uid)
{
    communication_send_event_t evt = { uid, -1 };
    postEvent<app_event_id_t::COMMUNICATION_SEND>(ctx->events, evt);
}

// Запись runtime-поля с diff+push: обёртка над writeFieldDetectChange —
// домен берётся из meta (владелец пишет поле своего домена), сравнение
// старого/нового и лок — внутри реестра (одна атомарная операция).
// Если реально изменилось — пушит поле клиентам (PARAM_PUSH broadcast).
// Возвращает true при изменении; ошибки записи (READONLY_DENIED /
// OUT_OF_RANGE / UNKNOWN_UID) возвращаются наружу как false + лог.
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

    const FieldMeta* m = ctx->fields.getMetaByUid(uid);
    if (!m)
    {
        ESP_LOGW("LogicUtils", "updateField: unknown uid=0x%04X", uid);
        return false;
    }

    bool changed = false;
    const FieldWriteStatus st = ctx->fields.writeFieldDetectChange(
        uid, &value, sizeof(T), m->domain, &changed);
    if (st != FieldWriteStatus::OK)
    {
        ESP_LOGW("LogicUtils", "updateField uid=0x%04X failed, st=%d",
                 uid, static_cast<int>(st));
        return false;
    }

    if (changed)
        sendField(ctx, uid);
    return changed;
}

// Опубликовать изменение поля в общей шине приложения.
// Позволяет доменным функциям общаться друг с другом через тот же
// механизм, что и запись по протоколу (app_event_id_t::CONFIG_CHANGED).
// Возврат post не проверяется — дроп логируется в EventManager.
inline void postFieldChanged(AppContext* ctx, uint16_t uid)
{
    field_change_event_t evt = { uid };
    postEvent<app_event_id_t::CONFIG_CHANGED>(ctx->events, evt);
}