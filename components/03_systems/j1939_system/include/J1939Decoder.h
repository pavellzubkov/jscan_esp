#pragma once
#include "TwaiDriver.h"
#include <cstdint>

// Результат декодирования 29-бит CAN ID в поля J1939.
struct J1939PgnMsg {
    uint32_t pgn;      // чистый PGN (без SA/PS для peer-to-peer)
    uint8_t  priority; // 0..7
    uint8_t  sa;       // source address
    uint8_t  dst;      // dest address (0xFF = broadcast/не применимо)
    uint8_t  dlc;
    uint8_t  data[8];
    bool     isP2P;
};

// Декодер 29-бит CAN ID → PGN/priority/SA/dst (порт twaiToj1939Short).
class J1939Decoder {
public:
    // 29-бит CAN ID → J1939-поля. Данные копируются по dlc.
    static J1939PgnMsg decode(const TwaiDriver::RxFrame& frame);

    // Диапазоны peer-to-peer (PDU1): 0 < PGN <= 0xEFFF или 0x10000 < PGN <= 0x1EFFF.
    static bool peerToPeer(uint32_t pgn);
};