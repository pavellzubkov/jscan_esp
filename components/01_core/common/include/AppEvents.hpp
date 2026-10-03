#pragma once
#include "EventManager.hpp"
#include "esp_event_base.h"
#include "J1939Proto.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// Определение base — одно, в src/AppEvents.cpp (ESP_EVENT_DEFINE_BASE).
// В заголовке — только DECLARE: define в .h давал бы своё определение в
// каждом TU (в C++ const имеет внутреннюю связь), а esp_event сравнивает
// base по УКАЗАТЕЛЮ — рассинхрон адресов молча ломал бы доставку событий.
ESP_EVENT_DECLARE_BASE(APP_EVENTS_BASE);

enum class app_event_id_t : int32_t {
    J1939_SNAPSHOT_SEND,    // данные: j1939_snapshot_t (postSized), байты батча
    J1939_REQUEST,          // данные: j1939_request_t — команда «послать RQST»
    WS_MESSAGE_RECEIVED,    // данные: ws_message_t (входящее WS-сообщение)
    WS_MESSAGE_SEND,        // данные: ws_message_t (исходящее WS-сообщение)
    WS_CLIENT_CONNECTED,    // данные: ws_message_t (sockfd только)
    WS_CLIENT_DISCONNECTED, // данные: ws_message_t (sockfd только)
    WIFI_STATUS,            // данные: wifi_status_event_t
    CONFIG_CHANGED,         // поле изменилось (данные: field_change_event_t, uid)
    WIFI_REAPPLY,           // отложенное применение конфига AP (без данных)
    COMMUNICATION_SEND,     // отправить поле WS-клиентам (данные: communication_send_event_t)
    FACTORY_RESET,          // сброс к заводским настройкам (без данных)
    OTA_BEGIN,              // начало OTA: flash-операции идут (без данных)
    OTA_END                 // конец OTA: flash-операции завершены (без данных)
};

// Максимальная длина WS-сообщения = лимит батча снапшота (см. J1939Proto).
constexpr size_t kMaxWsMessageLen = J1939Proto::kMaxBatchPayload;

// Входящее WS-сообщение: лимит запросов клиентов (PARAM_SET/REQUEST).
constexpr size_t kMaxWsInboundLen = 1024;

struct ws_message_t {
    int  sockfd;   // -1 = broadcast
    size_t length;
    char data[];   // flexible array
};

// Байты батча снапшота от J1939System → CommunicationModule (postSized).
struct j1939_snapshot_t {
    size_t length;
    uint8_t data[];
};

// Команда клиента: запросить PGN по J1939 (RQST, PGN 59904).
struct j1939_request_t {
    uint8_t  dstAddr;   // адрес назначения (адрес запрашиваемого узла)
    uint32_t pgn;       // запрашиваемый PGN (напр. 65227/65228)
};

struct wifi_status_event_t {
    bool    is_ap_mode;
    bool    is_connected;
    uint8_t num_clients;
    int8_t  rssi;
    uint8_t disconnect_reason;
};

// Данные для COMMUNICATION_SEND: поле, которое нужно отправить WS-клиентам.
struct communication_send_event_t {
    uint16_t fieldUid;   // UID поля из реестра (AppData)
    int      sockfd = -1; // адресная отправка (-1 = broadcast)
};

// Данные для CONFIG_CHANGED: UID изменённого поля.
struct field_change_event_t {
    uint16_t uid;   // UID изменённого поля
};

// === Typed pub/sub: compile-time проверка «пост ↔ подписка» ============
//
// Раньше post()/subscribe() дедуцировали тип payload с КАЖДОЙ стороны
// отдельно: рассинхрон по одному (base,id) компилировался и давал UB
// (обработчик получал указатель на чужой тип). Теперь тип берётся из
// app_event_id_t через traits AppEventPayload<Id>; первичный шаблон
// намеренно не определён — для не описанного события post/subscribe
// не соберутся вовсе.
//
// Sized-события (flexible array, postSized) помечены
// `static constexpr bool sized = true`.
template <app_event_id_t Id>
struct AppEventPayload;   // undefined primary

template <>
struct AppEventPayload<app_event_id_t::J1939_SNAPSHOT_SEND> {
    using type = j1939_snapshot_t;
    static constexpr bool sized = true;
};
template <>
struct AppEventPayload<app_event_id_t::J1939_REQUEST> {
    using type = j1939_request_t;
    static constexpr bool sized = false;
};
template <>
struct AppEventPayload<app_event_id_t::WS_MESSAGE_RECEIVED> {
    using type = ws_message_t;
    static constexpr bool sized = true;
};
template <>
struct AppEventPayload<app_event_id_t::WS_MESSAGE_SEND> {
    using type = ws_message_t;
    static constexpr bool sized = true;
};
template <>
struct AppEventPayload<app_event_id_t::WS_CLIENT_CONNECTED> {
    using type = ws_message_t;
    static constexpr bool sized = false;   // пост по значению: только sockfd
};
template <>
struct AppEventPayload<app_event_id_t::WS_CLIENT_DISCONNECTED> {
    using type = ws_message_t;
    static constexpr bool sized = false;   // пост по значению: только sockfd
};
template <>
struct AppEventPayload<app_event_id_t::WIFI_STATUS> {
    using type = wifi_status_event_t;
    static constexpr bool sized = false;
};
template <>
struct AppEventPayload<app_event_id_t::CONFIG_CHANGED> {
    using type = field_change_event_t;
    static constexpr bool sized = false;
};
template <>
struct AppEventPayload<app_event_id_t::WIFI_REAPPLY> {
    using type = void;
    static constexpr bool sized = false;
};
template <>
struct AppEventPayload<app_event_id_t::COMMUNICATION_SEND> {
    using type = communication_send_event_t;
    static constexpr bool sized = false;
};
template <>
struct AppEventPayload<app_event_id_t::FACTORY_RESET> {
    using type = void;
    static constexpr bool sized = false;
};
template <>
struct AppEventPayload<app_event_id_t::OTA_BEGIN> {
    using type = void;
    static constexpr bool sized = false;
};
template <>
struct AppEventPayload<app_event_id_t::OTA_END> {
    using type = void;
    static constexpr bool sized = false;
};

// --- Пост события С данными (копия в очередь event loop) ----------------
// T сверяется с trait: рассинхрон = static_assert, а не UB в рантайме.
// Sized-события сюда не передаются — только через postSizedEvent.
template <app_event_id_t Id, typename T>
bool postEvent(EventManager& em, const T& data) {
    using Expected = typename AppEventPayload<Id>::type;
    static_assert(!std::is_void_v<Expected>,
                  "event has no payload: use postEvent<Id>(em)");
    static_assert(!AppEventPayload<Id>::sized,
                  "sized event: use postSizedEvent<Id>(em, ptr, len)");
    static_assert(std::is_same_v<std::remove_cv_t<T>, Expected>,
                  "payload type mismatch: post type differs from "
                  "AppEventPayload<Id>::type");
    return em.post(APP_EVENTS_BASE, Id, data);
}

// --- Пост события БЕЗ данных -------------------------------------------
template <app_event_id_t Id>
bool postEvent(EventManager& em) {
    static_assert(std::is_void_v<typename AppEventPayload<Id>::type>,
                  "event carries a payload: use postEvent<Id>(em, data)");
    return em.post(APP_EVENTS_BASE, Id);
}

// --- Пост события с явным размером (flexible array) ---------------------
template <app_event_id_t Id, typename T>
bool postSizedEvent(EventManager& em, const T* data, size_t size) {
    using Expected = typename AppEventPayload<Id>::type;
    static_assert(AppEventPayload<Id>::sized,
                  "event is not sized: use postEvent<Id>(em, data)");
    static_assert(std::is_same_v<std::remove_cv_t<T>, Expected>,
                  "payload type mismatch: postSized type differs from "
                  "AppEventPayload<Id>::type");
    return em.postSized(APP_EVENTS_BASE, Id, data, size);
}

// --- Подписка на событие С данными (const T*) ---------------------------
// Тип T берётся из Id (non-deduced context): несовпадение метода с trait —
// substitution failure = ошибка компиляции, а не UB в рантайме.
template <app_event_id_t Id, typename Obj, typename Ret>
bool subscribeEvent(EventManager& em, Ret (Obj::*method)(
                        const typename AppEventPayload<Id>::type*),
                    Obj* obj) {
    static_assert(!std::is_void_v<typename AppEventPayload<Id>::type>,
                  "event has no payload: subscribe a method without args");
    return em.subscribe(APP_EVENTS_BASE, Id, method, obj);
}

// --- Подписка на событие БЕЗ данных (метод без аргументов) --------------
template <app_event_id_t Id, typename Obj, typename Ret>
bool subscribeEvent(EventManager& em, Ret (Obj::*method)(), Obj* obj) {
    static_assert(std::is_void_v<typename AppEventPayload<Id>::type>,
                  "event carries a payload: subscribe void(const T*) method");
    return em.subscribe(APP_EVENTS_BASE, Id, method, obj);
}