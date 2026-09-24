#pragma once
#include "esp_event_base.h"
#include <cstddef>
#include <cstdint>

ESP_EVENT_DEFINE_BASE(APP_EVENTS_BASE);

enum class app_event_id_t : int32_t {
    J1939_SNAPSHOT_SEND,    // данные: j1939_snapshot_t (postSized), байты батча
    J1939_REQUEST,          // данные: j1939_request_t — команда «послать RQST»
    WS_MESSAGE_RECEIVED,    // данные: ws_message_t (входящее WS-сообщение)
    WS_MESSAGE_SEND,        // данные: ws_message_t (исходящее WS-сообщение)
    WS_CLIENT_CONNECTED,    // данные: ws_message_t (sockfd только)
    WS_CLIENT_DISCONNECTED, // данные: ws_message_t (sockfd только)
    WIFI_STATUS,            // данные: wifi_status_event_t
    CONFIG_CHANGED          // данные: config_changed_event_t (задел)
};

// Максимальная длина WS-сообщения (батч снапшота до 8 КБ).
constexpr size_t kMaxWsMessageLen = 8192;
// Максимальный батч-пейлоад (payload кадра J1939_SNAPSHOT).
constexpr size_t kMaxBatchPayload = 8192;

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

struct config_changed_event_t {
    int field;   // индекс поля в AppConfig (перечислить в AppConfig.h при надобности)
};