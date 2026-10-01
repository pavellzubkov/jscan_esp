#pragma once
#include "J1939Proto.h"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>

// Динамическая карта активных PGN. Ключ — (sa, pgn).
//
// Память: короткие сообщения (<= 8 байт, одиночные кадры) хранятся inline
// в smallData; длинные (TP-реасемблер, до 1785 байт) — в динамическом буфере.
// Это отличается от исходного плана (массив kJ1939MaxDataLen на запись):
// 128 * 1785 байт не влезает в DRAM ESP32 без PSRAM.
//
// Владение bigData инкапсулировано в Record (правило пяти): деструктор
// освобождает буфер, копирование запрещено (double-free), перемещение
// обнуляет источник. Ручные free(bigData) по коду больше не нужны —
// вытеснение/инвалидация делаются через reset(), уплотнение — через move.
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
        uint8_t  smallData[8] = {};
        uint8_t* bigData = nullptr;

        const uint8_t* data() const { return bigData ? bigData : smallData; }

        Record() = default;
        ~Record() { free(bigData); }              // RAII: буфер освобождается сам

        Record(const Record&) = delete;            // copy → double-free
        Record& operator=(const Record&) = delete;

        Record(Record&& o) noexcept { *this = std::move(o); }
        Record& operator=(Record&& o) noexcept {
            if (this != &o) {
                free(bigData);
                pgn = o.pgn;
                sa = o.sa;
                len = o.len;
                lastTsMs = o.lastTsMs;
                periodMs = o.periodMs;
                valid = o.valid;
                for (size_t i = 0; i < sizeof(smallData); ++i)
                    smallData[i] = o.smallData[i];
                bigData = o.bigData;              // перенос владения
                o.bigData = nullptr;              // источник обнулён
            }
            return *this;
        }

        // Освободить bigData (слот остаётся валидным — просто inline-путь).
        void freeBig() noexcept {
            free(bigData);
            bigData = nullptr;
        }

        // Освободить буфер и обнулить запись → слот свободен.
        void reset() noexcept {
            freeBig();
            pgn = 0;
            sa = 0;
            len = 0;
            lastTsMs = 0;
            periodMs = 0;
            valid = false;
        }
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