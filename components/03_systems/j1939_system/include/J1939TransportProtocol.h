#pragma once
#include "J1939Decoder.h"
#include "J1939Proto.h"
#include <cstdint>

// Собранное много-пакетное сообщение (до 1785 байт).
struct J1939AssembledMsg {
    uint32_t pgn;
    uint8_t  sa;
    uint16_t len;              // реальная длина данных
    uint8_t  data[J1939Proto::kJ1939MaxDataLen];
};

// Действие, которое TP-слой просит отправить на шину (CTS/EOM).
// Слой 03 не знает TWAI/задач — J1939System транслирует в twai_.transmit,
// поэтому класс остаётся тестируемым на host.
struct TpAction {
    enum class Kind : uint8_t {
        None,      // отправлять нечего
        SendCts,   // RTS на наш адрес: подтверждаем приём
        SendEom,   // последний DT принят (только активный приём, не BAM/пассив)
    };
    Kind    kind         = Kind::None;
    uint8_t dst          = 0;    // SA адресата ответа (отправитель RTS)
    uint8_t packets      = 0;    // CTS: сколько DT-пакетов принимаем
    uint16_t totalLen    = 0;    // message size (CTS/EOM)
    uint8_t  totalPackets = 0;   // EOM: всего пакетов
    uint32_t pgn         = 0;    // пересылаемый PGN
};

// Реасемблер TP.CM/TP.DT (SAE J1939-21):
//  - BAM (control 32, broadcast) — сборка без ответов;
//  - RTS (16) → активный приём (dst == localAddr) с ответом CTS,
//    либо пассивный сниффинг чужого обмена (без ответов);
//  - EOM (19)/Abort (255) — закрытие сессии;
//  - CTS (17) — ignore: мы не передаём данные через TP.
// Таймауты — через tick(nowMs) звать из taskLoop: sweep не привязан
// к приходу DT (раньше сессия висела вечно, если DT прекратились).
class J1939TransportProtocol {
public:
    static constexpr uint8_t kMaxSessions = 3;   // BAM + активный RTS + пассивный RTS

    // Наш адрес узла (canNodeAddr) — распознавание RTS «на нас».
    // Вызвать до старта задачи (J1939System::begin) и при смене адреса.
    void setLocalAddr(uint8_t addr) { localAddr_ = addr; }

    void reset();

    // TP.CM: проверяет dlc, control byte, валидирует len/packets;
    // при необходимости создаёт/закрывает сессию. nowMs — Timing::nowMs().
    TpAction onTpCm(const J1939PgnMsg& msg, uint32_t nowMs);

    // TP.DT: сборка по 7 байт/пакет. Возвращает true, когда сборка
    // завершена (тогда out заполнен). action (если не nullptr) — EOM
    // для активного приёма. dlc < 2 — игнор.
    bool onTpDt(const J1939PgnMsg& msg, uint32_t nowMs,
                J1939AssembledMsg& out, TpAction* action = nullptr);

    // Sweep таймаутов всех сессий (kTransportTimeoutMs с последнего DT/CM).
    // Звать из taskLoop: DT могли прекратиться полностью.
    void tick(uint32_t nowMs);

private:
    // Контрол-байты TP.CM (SAE J1939-21).
    static constexpr uint8_t kCmRts   = 16;
    static constexpr uint8_t kCmCts   = 17;
    static constexpr uint8_t kCmEom   = 19;
    static constexpr uint8_t kCmBam   = 32;
    static constexpr uint8_t kCmAbort = 255;

    struct Session {
        bool     active = false;
        uint32_t pgn = 0;          // пересылаемый PGN (из CM)
        uint8_t  sa = 0;           // отправитель данных
        uint8_t  dst = 0xFF;       // адресат (0xFF = BAM/broadcast)
        uint16_t totalLen = 0;
        uint8_t  totalPackets = 0;
        uint8_t  packetsRemaining = 0;
        uint8_t  expectedPacket = 1;
        uint32_t lastMs = 0;       // Timing::nowMs() последнего активного события
        uint8_t  data[J1939Proto::kJ1939MaxDataLen];
    };

    // Валидация общих полей BAM/RTS: len in 1..1785, packets == ceil(len/7).
    static bool parseAnnounce(const J1939PgnMsg& msg,
                              uint16_t& totalLen, uint8_t& packets, uint32_t& pgn);
    // Перезапуск сессии того же sa + выделение слота (nullptr — пул полон).
    Session* startSession(uint8_t sa, uint8_t dst, uint16_t totalLen,
                          uint8_t packets, uint32_t pgn, uint32_t nowMs);

    Session sessions_[kMaxSessions];
    uint8_t localAddr_ = 0xFF;   // до setLocalAddr пассивный режим
};