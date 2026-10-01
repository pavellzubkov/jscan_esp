#pragma once
#include "AppContext.h"
#include "AppEvents.h"
#include "FrameTx.hpp"
#include "J1939Channel.hpp"
#include <cstdint>

// Протокольный слой: кадр (magic/ver/flags/MsgType/len/seq/CRC), диспатч команд
// и канал параметров (MsgType 0x0003..0x0008). J1939-ветка протокола
// (снапшоты 0x0001, запрос PGN 0x0002) и аудитория WS — в подмодуле
// J1939Channel. Транспорт (WS) — через события WS_MESSAGE_*.
// Без отдельной задачи: обработчики лёгкие (копии ~8 КБ).
class CommunicationModule {
public:
    explicit CommunicationModule(AppContext* ctx);
    ~CommunicationModule();   // снять подписки (нужно и при откате begin())

    esp_err_t begin();

private:
    AppContext* ctx_;
    // Порядок членов важен: tx_ кодирует кадры для j1939_ и должен жить
    // дольше подписок подмодуля (dtor CommModule снимает подписки своим
    // телом, затем члены уничтожаются в обратном порядке).
    FrameTx tx_;             // общий кодировщик кадров (wrapFrame + seq)
    J1939Channel j1939_;     // J1939-ветка: аудитория WS + гейт снапшотов

    void onIncomingPacket(const ws_message_t* msg);
    void onWsClientConnected(const ws_message_t* msg);
    void onWsClientDisconnected(const ws_message_t* msg);
    void onWifiStatus(const wifi_status_event_t* s);
    void onCommunicationSend(const communication_send_event_t* evt);

    // --- Канал параметров ---
    void sendValueFrame(uint16_t msgType, uint16_t uid, int sockfd);
    void sendNack(uint16_t uid, uint8_t err, int sockfd);
};
