#include "J1939TransportProtocol.h"
#include "SystemTiming.h"
#include <cstring>
#include <esp_log.h>

static const char* TAG_TP = "TP";

void J1939TransportProtocol::reset()
{
    for (auto& s : sessions_)
        s.active = false;
}

void J1939TransportProtocol::onTpCm(const J1939PgnMsg& msg)
{
    // Только BAM: control byte 32 и широковещательная рассылка (dst = 0xFF)
    if (msg.data[0] != 32 || msg.dst != 0xFF)
        return;

    // Формат TP.CM (BAM): data[1..2] — длина, data[3] — число пакетов,
    // data[5..7] — PGN (little-endian).
    uint16_t totalLen = static_cast<uint16_t>(msg.data[1] | (msg.data[2] << 8));
    uint8_t  packets  = msg.data[3];
    uint32_t pgn      = static_cast<uint32_t>(msg.data[5]) |
                        (static_cast<uint32_t>(msg.data[6]) << 8) |
                        (static_cast<uint32_t>(msg.data[7]) << 16);

    if (totalLen == 0 || totalLen > J1939Proto::kJ1939MaxDataLen)
        return;

    // Валидация числа пакетов: по спецификации packets == ceil(totalLen/7).
    // packets == 0 далее превратил бы packetsRemaining-- в 255 (uint8_t) —
    // сессия висела бы до таймаута и «съедала» бы слот.
    const uint8_t expectedPackets =
        static_cast<uint8_t>((totalLen + 6) / 7);   // ceil(len/7), max 255
    if (packets == 0 || packets != expectedPackets)
    {
        ESP_LOGD(TAG_TP, "TP.CM rejected: packets=%u expected=%u len=%u",
                 unsigned(packets), unsigned(expectedPackets), unsigned(totalLen));
        return;   // некорректный BAM — сессия не создаётся
    }

    // Старая незавершённая сессия того же узла — перезапустить
    for (auto& s : sessions_)
    {
        if (s.active && s.sa == msg.sa)
            s.active = false;
    }

    for (auto& s : sessions_)
    {
        if (s.active)
            continue;
        s.active = true;
        s.pgn = pgn;
        s.sa = msg.sa;
        s.totalLen = totalLen;
        s.packetsRemaining = packets;
        s.expectedPacket = 1;
        s.lastTs = xTaskGetTickCount();
        return;
    }
}

bool J1939TransportProtocol::onTpDt(const J1939PgnMsg& msg, J1939AssembledMsg& out)
{
    const TickType_t now = xTaskGetTickCount();
    const TickType_t timeoutTicks = pdMS_TO_TICKS(Timing::kTransportTimeoutMs);

    // Сбросить сессии с истёкшим таймаутом (без переполняющегося сравнения)
    for (auto& s : sessions_)
    {
        if (s.active && (now - s.lastTs) >= timeoutTicks)
            s.active = false;
    }

    for (auto& s : sessions_)
    {
        if (!s.active || s.sa != msg.sa)
            continue;

        const uint8_t packetN = msg.data[0];
        if (packetN != s.expectedPacket)
            return false;   // потерянный пакет или дубль — ждём нужный номер

        const size_t off = static_cast<size_t>(packetN - 1) * 7;
        size_t chunk = (msg.dlc > 1) ? (msg.dlc - 1) : 0;
        if (off + chunk > s.totalLen)
            chunk = (s.totalLen > off) ? (s.totalLen - off) : 0;   // последний пакет
        if (off + chunk > J1939Proto::kJ1939MaxDataLen)
        {
            s.active = false;
            return false;
        }

        memcpy(&s.data[off], &msg.data[1], chunk);
        s.expectedPacket++;
        s.packetsRemaining--;
        s.lastTs = now;

        if (s.packetsRemaining == 0)
        {
            s.active = false;
            out.pgn = s.pgn;
            out.sa = s.sa;
            out.len = s.totalLen;
            memcpy(out.data, s.data, s.totalLen);
            return true;
        }
        return false;
    }
    return false;
}