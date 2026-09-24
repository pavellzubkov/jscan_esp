#pragma once
#include "AppContext.h"
#include "AppEvents.h"
#include <cstdint>

// Протокольный слой: кадр (magic/ver/flags/MsgType/len/seq/CRC), диспатч команд,
// обёртка батча снапшота в кадр. Транспорт (WS) — через события WS_MESSAGE_*.
// Без отдельной задачи: обработчики лёгкие (копии ~8 КБ).
class CommunicationModule {
public:
    explicit CommunicationModule(AppContext* ctx);

    esp_err_t begin();

private:
    AppContext* ctx_;
    uint16_t tx_seq_ = 0;   // монотонный счётчик исходящих кадров

    void onIncomingPacket(const ws_message_t* msg);
    void onSnapshot(const j1939_snapshot_t* snap);
    void onWsClientConnected(const ws_message_t* msg);
    void onWsClientDisconnected(const ws_message_t* msg);
    void onWifiStatus(const wifi_status_event_t* s);
};