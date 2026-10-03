#pragma once
#include "AppContext.hpp"
#include <cstddef>
#include <cstdint>

// Кодировщик исходящих кадров протокола (magic/ver/flags/MsgType/len/seq/
// payload/CRC16): payload → wrapFrame → ws_message → событие WS_MESSAGE_SEND.
// Общий для обоих каналов коммуникации (параметры CommModule и снапшоты
// J1939Channel) — одна последовательность seq на все исходящие кадры.
// Вынесен из CommunicationModule, чтобы подмодуль не зависел от модуля-владельца.
class FrameTx {
public:
    explicit FrameTx(AppContext* ctx) : ctx_(ctx) {}
    FrameTx(const FrameTx&) = delete;
    FrameTx& operator=(const FrameTx&) = delete;

    // sockfd=-1 → broadcast всем клиентам, иначе адресная отправка сокету.
    void send(uint16_t msgType, uint8_t flags, const uint8_t* payload,
              size_t payloadLen, int sockfd);

private:
    AppContext* ctx_;
    uint16_t seq_ = 0;   // монотонный счётчик исходящих кадров
};
