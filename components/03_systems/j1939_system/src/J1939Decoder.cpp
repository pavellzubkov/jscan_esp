#include "J1939Decoder.hpp"

bool J1939Decoder::peerToPeer(uint32_t pgn)
{
    // PDU1 (peer-to-peer): PF < 240 — критерий покрывает все страницы DP/R,
    // включая pgn==0 (PF=0, dst=0) и DP=1 (0x10000+).
    return ((pgn >> 8) & 0xFF) < 0xF0;
}

J1939PgnMsg J1939Decoder::decode(const TwaiDriver::RxFrame& frame)
{
    J1939PgnMsg m = {};
    const uint32_t id = frame.id;

    // Приоритет — биты 26..28
    m.priority = static_cast<uint8_t>((id >> 26) & 0x07);
    // PGN — биты 8..25 (18 бит), PS (биты 8..15) для PDU1 — адрес назначения
    uint32_t pgnRaw = (id >> 8) & 0x3FFFF;
    m.sa = static_cast<uint8_t>(id & 0xFF);

    m.isP2P = peerToPeer(pgnRaw);
    if (m.isP2P)
    {
        m.dst = static_cast<uint8_t>(pgnRaw & 0xFF);
        pgnRaw &= 0x3FFF00;   // убрать PS, оставить PF/DP/R (R — бит 17)
    }
    else
    {
        m.dst = 0xFF;   // broadcast/не применимо
        // PDU2: PS — часть PGN, pgnRaw не трогаем
    }
    m.pgn = pgnRaw;

    m.dlc = (frame.dlc > 8) ? 8 : frame.dlc;
    for (uint8_t i = 0; i < m.dlc; ++i)
        m.data[i] = frame.data[i];

    return m;
}