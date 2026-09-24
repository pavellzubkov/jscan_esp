#pragma once
#include "J1939Decoder.h"
#include "J1939Proto.h"
#include "freertos/FreeRTOS.h"
#include <cstdint>

// Собранное много-пакетное сообщение (до 1785 байт).
struct J1939AssembledMsg {
    uint32_t pgn;
    uint8_t  sa;
    uint16_t len;              // реальная длина данных
    uint8_t  data[J1939Proto::kJ1939MaxDataLen];
};

// Реасемблер TP.CM (BAM) + TP.DT для широковещательных (BAM) сообщений.
// Порт TPStartWriting/TPWriting из esp_j1939/j1939twai.cpp, переписанный
// на J1939PgnMsg и с исправлением переполняющегося сравнения тиков.
class J1939TransportProtocol {
public:
    static constexpr uint8_t kMaxSessions = 2;   // параллельных BAM-сборок

    void reset();

    // TP.CM: control byte 32 = BAM (broadcast announcement).
    void onTpCm(const J1939PgnMsg& msg);

    // TP.DT: пакет данных (7 байт полезных + номер пакета в data[0]).
    // Возвращает true, когда сборка завершена; тогда результат в out.
    bool onTpDt(const J1939PgnMsg& msg, J1939AssembledMsg& out);

private:
    struct Session {
        bool     active = false;
        uint32_t pgn = 0;
        uint8_t  sa = 0;
        uint16_t totalLen = 0;
        uint8_t  packetsRemaining = 0;   // сколько пакетов ещё ждать
        uint8_t  expectedPacket = 1;     // следующий номер пакета
        TickType_t lastTs = 0;
        uint8_t  data[J1939Proto::kJ1939MaxDataLen];   // буфер сборки
    };
    Session sessions_[kMaxSessions];
};