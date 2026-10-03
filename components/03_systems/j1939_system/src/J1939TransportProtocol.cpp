#include "J1939TransportProtocol.hpp"
#include "HardwareConfig.hpp"
#include "SystemTiming.hpp"
#include <cstring>
#include <esp_log.h>

static const char* TAG_TP = "TP";

void J1939TransportProtocol::reset()
{
    for (auto& s : sessions_)
        s.active = false;
}

bool J1939TransportProtocol::parseAnnounce(const J1939PgnMsg& msg,
                                           uint16_t& totalLen, uint8_t& packets,
                                           uint32_t& pgn)
{
    // Формат TP.CM (BAM/RTS): data[1..2] — длина LE, data[3] — число пакетов,
    // data[5..7] — PGN LE. dlc >= 8 уже проверен вызывающим.
    totalLen = static_cast<uint16_t>(msg.data[1] | (msg.data[2] << 8));
    packets  = msg.data[3];
    pgn      = static_cast<uint32_t>(msg.data[5]) |
               (static_cast<uint32_t>(msg.data[6]) << 8) |
               (static_cast<uint32_t>(msg.data[7]) << 16);

    if (totalLen == 0 || totalLen > J1939Proto::kJ1939MaxDataLen)
        return false;

    // Валидация числа пакетов: по спецификации packets == ceil(totalLen/7).
    // packets == 0 далее превратил бы packetsRemaining-- в 255 (uint8_t) —
    // сессия висела бы до таймаута и «съедала» бы слот.
    const uint8_t expectedPackets =
        static_cast<uint8_t>((totalLen + 6) / 7);   // ceil(len/7), max 255
    if (packets == 0 || packets != expectedPackets)
    {
        ESP_LOGD(TAG_TP, "TP.CM rejected: packets=%u expected=%u len=%u",
                 unsigned(packets), unsigned(expectedPackets), unsigned(totalLen));
        return false;   // некорректное объявление — сессия не создаётся
    }
    return true;
}

J1939TransportProtocol::Session* J1939TransportProtocol::startSession(
    uint8_t sa, uint8_t dst, uint16_t totalLen, uint8_t packets,
    uint32_t pgn, uint32_t nowMs)
{
    // Старая незавершённая сессия того же отправителя — перезапустить
    for (auto& s : sessions_)
    {
        if (s.active && s.sa == sa)
            s.active = false;
    }

    for (auto& s : sessions_)
    {
        if (s.active)
            continue;
        s.active = true;
        s.pgn = pgn;
        s.sa = sa;
        s.dst = dst;
        s.totalLen = totalLen;
        s.totalPackets = packets;
        s.packetsRemaining = packets;
        s.expectedPacket = 1;
        s.lastMs = nowMs;
        return &s;
    }
    ESP_LOGD(TAG_TP, "session pool exhausted (%u)", unsigned(kMaxSessions));
    return nullptr;
}

TpAction J1939TransportProtocol::onTpCm(const J1939PgnMsg& msg, uint32_t nowMs)
{
    TpAction act;   // Kind::None по умолчанию

    // Короткий/битый CM: data[1..7] при dlc<8 — мусор. Не трогаем сессии.
    if (msg.dlc < 8)
        return act;

    const uint8_t control = msg.data[0];
    switch (control)
    {
    case kCmBam:
    {
        // BAM — только широковещательная рассылка (dst = 0xFF).
        if (msg.dst != 0xFF)
            return act;

        uint16_t totalLen = 0; uint8_t packets = 0; uint32_t pgn = 0;
        if (!parseAnnounce(msg, totalLen, packets, pgn))
            return act;

        startSession(msg.sa, 0xFF, totalLen, packets, pgn, nowMs);
        return act;   // BAM — ответов не требует
    }

    case kCmRts:
    {
        // RTS — peer-to-peer (dst из PS, PDU1). Общий валид как у BAM.
        uint16_t totalLen = 0; uint8_t packets = 0; uint32_t pgn = 0;
        if (!parseAnnounce(msg, totalLen, packets, pgn))
            return act;

        if (msg.dst == localAddr_)
        {
            // Активный приём: нас просят CTS. Принимаем всё разом —
            // буфер сессии вмещает 1785 байт (max 255 пакетов).
            startSession(msg.sa, msg.dst, totalLen, packets, pgn, nowMs);
            act.kind = TpAction::Kind::SendCts;
            act.dst = msg.sa;          // ответ отправителю RTS
            act.packets = packets;     // CTS byte1: сколько DT мы готовы принять
            act.totalLen = totalLen;
            act.totalPackets = packets;
            act.pgn = pgn;
            return act;
        }

        // Чужой обмен (dst != мы): пассивный сниффинг — собираем DT молча,
        // НЕ отвечаем (иначе на шине окажется второй «получатель»).
        startSession(msg.sa, msg.dst, totalLen, packets, pgn, nowMs);
        return act;
    }

    case kCmEom:
    {
        // EOM шлёт ПОЛУЧАТЕЛЬ последнего DT → отправителю: msg.sa = получатель,
        // msg.dst = отправитель = s.sa. Закрываем его сессию.
        const uint32_t pgn = static_cast<uint32_t>(msg.data[5]) |
                             (static_cast<uint32_t>(msg.data[6]) << 8) |
                             (static_cast<uint32_t>(msg.data[7]) << 16);
        for (auto& s : sessions_)
        {
            if (s.active && s.pgn == pgn && s.sa == msg.dst)
                s.active = false;
        }
        return act;
    }

    case kCmAbort:
    {
        // Abort шлёт любая сторона (отправитель получателю или наоборот).
        // Сессию ищем по PGN и участию SA в обоих ролях.
        const uint32_t pgn = static_cast<uint32_t>(msg.data[5]) |
                             (static_cast<uint32_t>(msg.data[6]) << 8) |
                             (static_cast<uint32_t>(msg.data[7]) << 16);
        for (auto& s : sessions_)
        {
            if (s.active && s.pgn == pgn &&
                (s.sa == msg.sa || s.sa == msg.dst))
                s.active = false;
        }
        return act;
    }

    case kCmCts:
    default:
        // CTS — мы через TP не передаём (только приём); прочие контролы —
        // неизвестные расширения, игнорируем.
        return act;
    }
}

bool J1939TransportProtocol::onTpDt(const J1939PgnMsg& msg, uint32_t nowMs,
                                    J1939AssembledMsg& out, TpAction* action)
{
    if (action)
        action->kind = TpAction::Kind::None;

    // data[0] — номер пакета; при dlc < 2 его нет (dlc==1 — только номер? по
    // спецификации DT = 1 номер + 7 данных; минимум осмысленного пакета — 2
    // байта: номер + >=1 данных; dlc<2 — мусор).
    if (msg.dlc < 2)
        return false;

    for (auto& s : sessions_)
    {
        if (!s.active || s.sa != msg.sa)
            continue;

        const uint8_t packetN = msg.data[0];
        if (packetN != s.expectedPacket)
            return false;   // потерянный пакет или дубль — ждём нужный номер

        const size_t off = static_cast<size_t>(packetN - 1) * 7;
        size_t chunk = msg.dlc - 1;
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
        s.lastMs = nowMs;

        if (s.packetsRemaining == 0)
        {
            s.active = false;
            out.pgn = s.pgn;
            out.sa = s.sa;
            out.len = s.totalLen;
            memcpy(out.data, s.data, s.totalLen);

            // Только АКТИВНЫЙ приём (dst == мы) отвечает EOM: BAM (dst=0xFF)
            // и пассивный сниффинг молчат — иначе на шине второй получатель.
            if (action && s.dst == localAddr_)
            {
                action->kind = TpAction::Kind::SendEom;
                action->dst = s.sa;          // EOM: получатель → отправитель
                action->totalLen = s.totalLen;
                action->totalPackets = s.totalPackets;
                action->pgn = s.pgn;
            }
            return true;
        }
        return false;
    }
    return false;
}

bool J1939TransportProtocol::buildCmPayload(const TpAction& act, uint8_t out[8])
{
    // Reserved-байты 0xFF по спецификации (SAE J1939-21).
    std::memset(out, 0xFF, 8);
    switch (act.kind)
    {
    case TpAction::Kind::SendCts:
        // CTS: [17, packets-we-can-receive, 0xFF(max), 0xFF, 0xFF, PGN LE]
        out[0] = kCmCts;
        out[1] = act.packets;
        break;
    case TpAction::Kind::SendEom:
        // EOM: [19, size LE, total packets, 0xFF, 0xFF, PGN LE]
        out[0] = kCmEom;
        out[1] = static_cast<uint8_t>(act.totalLen & 0xFF);
        out[2] = static_cast<uint8_t>((act.totalLen >> 8) & 0xFF);
        out[3] = act.totalPackets;
        break;
    case TpAction::Kind::None:
    default:
        return false;   // отправлять нечего — кадр не формируется
    }
    out[5] = static_cast<uint8_t>(act.pgn & 0xFF);
    out[6] = static_cast<uint8_t>((act.pgn >> 8) & 0xFF);
    out[7] = static_cast<uint8_t>((act.pgn >> 16) & 0xFF);
    return true;
}

uint32_t J1939TransportProtocol::buildCmId(const TpAction& act, uint8_t nodeAddr)
{
    // TP.CM — PDU1 (PF=0xEC): ID = prio<<26 | PGN<<8 (биты PF) |
    // PS(=dst)<<8 | SA(=nodeAddr); младшие 8 бит kPgnTpCm (PS) = 0.
    return (7u << 26) | (Hw::kPgnTpCm << 8) |
           (static_cast<uint32_t>(act.dst) << 8) | nodeAddr;
}

void J1939TransportProtocol::tick(uint32_t nowMs)
{
    // Сравнение через (int32_t) поверх uint32-счётчика мс — корректно при
    // переполнении (как в Timing::computeWaitMs). Sweep отдельно от onTpDt:
    // если DT прекратились совсем, сессия закрывается, а не висит вечно.
    const int32_t timeout = static_cast<int32_t>(Timing::kTransportTimeoutMs);
    for (auto& s : sessions_)
    {
        if (s.active && static_cast<int32_t>(nowMs - s.lastMs) >= timeout)
            s.active = false;
    }
}