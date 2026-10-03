#pragma once
#include "AppContext.hpp"
#include "AppEvents.hpp"
#include <cstddef>
#include <cstdint>

class FrameTx;

// J1939-ветка коммуникационного протокола (MsgType 0x0001 снапшоты исходящие,
// 0x0002 запрос PGN входящий). Владеет аудиторией WS-клиентов и решает,
// отправлять ли снапшоты вообще: J1939Scanner публикует J1939_SNAPSHOT_SEND
// безусловно, а здесь гейт «клиентов нет — не шлём».
// Осознанно отдельно от CommModule: канал параметров (0x0003..0x0008) и
// J1939-канал — разные протоколы с разной политикой доставки.
class J1939Channel {
public:
    // tx — общий кодировщик кадров владельца (CommModule): lifetime члена
    // длиннее, чем lifetime подписок (dtor снимает их первым делом).
    J1939Channel(AppContext* ctx, FrameTx* tx);
    ~J1939Channel();   // снять подписки (нужно и при откате begin() CommModule)

    esp_err_t begin();

    // Входящий MsgType 0x0002 (J1939_REQUEST): {dstAddr u8, pgn u32 LE}.
    // Вызывается CommModule после unwrapFrame — здесь парсинг и post
    // команды SCANNER_REQUEST (J1939Scanner → форвард в J1939System).
    void onClientRequest(const uint8_t* payload, size_t payloadLen);

private:
    AppContext* ctx_;
    FrameTx* tx_;
    // Число подключённых WS-клиентов: при 0 снапшоты не отправляются.
    // Все обработчики выполняются в одной event-loop задаче, поэтому обычный
    // int (без atomic): мутации и чтения не гоняются между потоками.
    int clients_ = 0;

    void onSnapshot(const j1939_snapshot_t* snap);
    void onClientConnected(const ws_message_t* msg);
    void onClientDisconnected(const ws_message_t* msg);
};
