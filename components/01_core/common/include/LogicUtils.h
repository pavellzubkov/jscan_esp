#pragma once
#include "AppContext.h"
#include "AppEvents.h"

// Отправка одного поля по коммуникационному модулю (broadcast)
inline void sendField(AppContext* ctx, uint16_t uid)
{
    communication_send_event_t evt = { uid, -1 };
    ctx->events.post(APP_EVENTS_BASE, app_event_id_t::COMMUNICATION_SEND, evt);
}

// Опубликовать изменение поля в общей шине приложения.
// Позволяет доменным функциям общаться друг с другом через тот же
// механизм, что и запись по протоколу (app_event_id_t::CONFIG_CHANGED).
inline void postFieldChanged(AppContext* ctx, uint16_t uid)
{
    field_change_event_t evt = { uid };
    ctx->events.post(APP_EVENTS_BASE, app_event_id_t::CONFIG_CHANGED, evt);
}