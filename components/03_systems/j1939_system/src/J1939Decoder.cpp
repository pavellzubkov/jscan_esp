#include "J1939Decoder.h"

bool J1939Decoder::peerToPeer(uint32_t pgn)
{
    // Проверка PGN на принадлежность PDU1 (peer-to-peer)
    if (pgn > 0 && pgn <= 0xEFFF)
        return true;
    if (pgn > 0x10000 && pgn <= 0x1EFFF)
        return true;
    return false;
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
        pgnRaw &= 0x1FF00;   // убрать PS, оставить PF/DP/EDP
    }
    else
    {
        m.dst = 0xFF;   // broadcast/не применимо
    }
    m.pgn = pgnRaw;

    m.dlc = (frame.dlc > 8) ? 8 : frame.dlc;
    for (uint8_t i = 0; i < m.dlc; ++i)
        m.data[i] = frame.data[i];

    return m;
}