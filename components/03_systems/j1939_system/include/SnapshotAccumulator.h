#pragma once
#include "J1939Proto.h"
#include <cstddef>
#include <cstdint>

// Динамическая карта активных PGN. Ключ — (sa, pgn).
//
// Память: короткие сообщения (<= 8 байт, одиночные кадры) хранятся inline
// в smallData; длинные (TP-реасемблер, до 1785 байт) — в динамическом буфере.
// Это отличается от исходного плана (массив kJ1939MaxDataLen на запись):
// 128 * 1785 байт не влезает в DRAM ESP32 без PSRAM.
//
// Конкурентность: единственный писатель/читатель — задача J1939System,
// поэтому мьютекс не нужен. Если появится второй читатель — обернуть в mutex.
class SnapshotAccumulator {
public:
    static constexpr size_t kMaxRecords = 128;   // AppConfig.maxTrackedPgns

    struct Record {
        uint32_t pgn = 0;
        uint8_t  sa = 0;
        uint16_t len = 0;
        uint32_t lastTsMs = 0;   // монотонный тик последнего обновления
        uint16_t periodMs = 0;   // наблюдаемый период (разница тиков)
        bool     valid = false;
        // Данные: короткие — inline, длинные (TP) — heap-буфер
        uint8_t  smallData[8];
        uint8_t* bigData = nullptr;

        const uint8_t* data() const { return bigData ? bigData : smallData; }
    };

    void update(uint32_t sa, uint32_t pgn,
                const uint8_t* data, size_t len, uint32_t nowMs);

    // Выгрузить «активные» записи (обновлявшиеся не дольше ttlMs назад) в out.
    // Активные записи уплотняются в начало records_ (меняет порядок).
    size_t collect(const Record*& out, uint32_t nowMs, uint32_t ttlMs);

    size_t count() const { return count_; }

private:
    Record records_[kMaxRecords];
    size_t count_ = 0;

    size_t findSlot(uint32_t sa, uint32_t pgn) const;          // kMaxRecords = нет
    size_t findFreeOrOldest(uint32_t nowMs) const;             // kMaxRecords = полна
    void   storeData(Record& r, const uint8_t* data, size_t len);
};