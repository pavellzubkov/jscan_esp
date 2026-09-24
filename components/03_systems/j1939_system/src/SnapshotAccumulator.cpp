#include "SnapshotAccumulator.h"
#include <cstdlib>
#include <cstring>

size_t SnapshotAccumulator::findSlot(uint32_t sa, uint32_t pgn) const
{
    for (size_t i = 0; i < kMaxRecords; ++i)
    {
        const Record& r = records_[i];
        if (r.valid && r.sa == static_cast<uint8_t>(sa) && r.pgn == pgn)
            return i;
    }
    return kMaxRecords;
}

size_t SnapshotAccumulator::findFreeOrOldest(uint32_t nowMs) const
{
    size_t oldest = kMaxRecords;
    uint32_t oldestTs = 0;
    for (size_t i = 0; i < kMaxRecords; ++i)
    {
        const Record& r = records_[i];
        if (!r.valid)
            return i;   // свободный слот
        if (oldest == kMaxRecords || r.lastTsMs < oldestTs)
        {
            oldest = i;
            oldestTs = r.lastTsMs;
        }
    }
    (void)nowMs;
    return oldest;   // карта полна — перезаписать самую старую
}

void SnapshotAccumulator::storeData(Record& r, const uint8_t* data, size_t len)
{
    if (len <= sizeof(r.smallData))
    {
        if (r.bigData)
        {
            free(r.bigData);
            r.bigData = nullptr;
        }
        memcpy(r.smallData, data, len);
        r.len = static_cast<uint16_t>(len);
        return;
    }

    // Длинное сообщение (TP) — держим heap-буфер точно по длине
    if (!r.bigData)
    {
        r.bigData = static_cast<uint8_t*>(malloc(len));
    }
    else if (r.len != len)
    {
        uint8_t* nb = static_cast<uint8_t*>(realloc(r.bigData, len));
        if (!nb)
            return;   // не удалось — оставляем предыдущие данные
        r.bigData = nb;
    }
    if (!r.bigData)
        return;
    memcpy(r.bigData, data, len);
    r.len = static_cast<uint16_t>(len);
}

void SnapshotAccumulator::update(uint32_t sa, uint32_t pgn,
                                 const uint8_t* data, size_t len,
                                 uint32_t nowMs)
{
    if (data == nullptr || len == 0 || len > J1939Proto::kJ1939MaxDataLen)
        return;

    size_t idx = findSlot(sa, pgn);
    if (idx == kMaxRecords)
    {
        idx = findFreeOrOldest(nowMs);
        if (idx == kMaxRecords)
            return;
        // Вытесняем запись — освобождаем её длинный буфер
        Record& victim = records_[idx];
        if (victim.valid && victim.bigData)
        {
            free(victim.bigData);
            victim.bigData = nullptr;
        }
        victim.pgn = pgn;
        victim.sa = static_cast<uint8_t>(sa);
        victim.lastTsMs = 0;
        victim.periodMs = 0;
        victim.valid = true;
        count_++;
    }

    Record& r = records_[idx];
    r.periodMs = (r.lastTsMs != 0)
                     ? static_cast<uint16_t>(nowMs - r.lastTsMs)
                     : 0;
    r.lastTsMs = nowMs;
    storeData(r, data, len);
}

size_t SnapshotAccumulator::collect(const Record*& out, uint32_t nowMs,
                                    uint32_t ttlMs)
{
    size_t n = 0;
    for (size_t i = 0; i < kMaxRecords; ++i)
    {
        const Record& r = records_[i];
        if (!r.valid)
            continue;
        if ((nowMs - r.lastTsMs) > ttlMs)
            continue;
        if (n != i)
            records_[n] = r;   // уплотняем активные в начало
        ++n;
    }
    out = records_;
    return n;
}