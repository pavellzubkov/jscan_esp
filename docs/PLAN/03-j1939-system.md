# STEP-03 — `03_systems/j1939_system`

> Статус: **done**

> Контекст: свежая сессия. STEP-01 (core) и STEP-02 (twai) выполнены, проект
> собирается. Этот шаг создаёт координатор J1939: декодер 29-бит CAN ID →
> PGN/SA, TP-реасемблер (BAM + DT), аккумулятор снапшота и единственную задачу
> приёма+публикации. **Старый `esp_j1939` не трогаем** (удалим в STEP-07).
> Проверка: `idf.py reconfigure && idf.py build`.

## Цель

`J1939System` (модуль, подключаемый позже в BootManager):
- владеет `TwaiDriver`, `J1939TransportProtocol`, `SnapshotAccumulator`;
- одна задача: читает кадры из TWAI-очереди с таймаутом до следующего снапшота,
  декодирует, реасемблирует TP, обновляет аккумулятор; по таймеру сериализует
  батч и публикует событие `J1939_SNAPSHOT_SEND`;
- подписка на `J1939_REQUEST` → шлёт RQST (PGN 59904).

## Структура

```
components/03_systems/j1939_system/
  CMakeLists.txt
  include/
    J1939Decoder.h
    J1939TransportProtocol.h
    SnapshotAccumulator.h
    J1939System.h
  src/
    J1939Decoder.cpp
    J1939TransportProtocol.cpp
    SnapshotAccumulator.cpp
    J1939System.cpp
```

## `CMakeLists.txt`

```cmake
idf_component_register(
    SRC_DIRS .
    INCLUDE_DIRS .
    REQUIRES common twai freertos
)
```

## `J1939Decoder.h` / `.cpp`

Порт `twaiToj1939Short` + `j1939PeerToPeer` из `esp_j1939/j1939twai.cpp`:

```cpp
#pragma once
#include "twai/TwaiDriver.h"
#include <cstdint>

struct J1939PgnMsg {
    uint32_t pgn;      // чистый PGN (без SA/PS для peer-to-peer)
    uint8_t  priority; // 0..7
    uint8_t  sa;       // source address
    uint8_t  dst;      // dest address (0xFF = broadcast/не применимо)
    uint8_t  dlc;
    uint8_t  data[8];
    bool     isP2P;
};

class J1939Decoder {
public:
    // 29-бит CAN ID → J1939-поля. Данные копируются по dlc.
    static J1939PgnMsg decode(const TwaiDriver::RxFrame& frame);
};
```

Правила (из старого кода, проверить на корректность):
- `priority = (id >> 26) & 0x07`;
- `pgn = (id >> 8) & 0x3FFFF`; для peer-to-peer (`j1939PeerToPeer`) битовая маска:
  `dst = pgn & 0xFF`, `pgn &= 0x1FF00` (убрать PS);
- `sa = id & 0xFF`;
- peer-to-peer диапазоны: `0 < pgn <= 0xEFFF` или `0x10000 < pgn <= 0x1EFFF`
  (перенести функцию как есть).

## `J1939TransportProtocol.h` / `.cpp`

Реасемблер `TP.CM` (BAM) + `TP.DT` для **широковещательных** (BAM) сообщений.
Порт `TPStartWriting`/`TPWriting` из `esp_j1939/j1939twai.cpp` (строки ~141–207),
переписанный на `J1939PgnMsg`:

```cpp
#pragma once
#include "J1939Decoder.h"
#include <cstdint>
#include "freertos/FreeRTOS.h"

// Собранное много-пакетное сообщение (до 1785 байт).
struct J1939AssembledMsg {
    uint32_t pgn;
    uint8_t  sa;
    uint16_t len;              // реальная длина данных
    uint8_t  data[1785];       // kJ1939MaxDataLen из J1939Proto.h
};

class J1939TransportProtocol {
public:
    static constexpr uint8_t kMaxSessions = 2;   // параллельных BAM-сборок

    void reset();

    // TP.CM: control byte 32 = BAM (broadcast announcement).
    // Заполняет сессию из nData (см. формат TP.CM в J1939).
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
    };
    Session sessions_[kMaxSessions];
};
```

Ключевые моменты (исправить грабли старого кода):
- время — монотонный `xTaskGetTickCount()`; таймаут сессии — `Timing::kTransportTimeoutMs`
  (из `SystemTiming.h`): если `now - lastTs >= timeout` — сбросить сессию;
- не использовать переполняющееся сравнение `(now - lastTs) < 0` (старый баг);
- `TP.DT` data[0] = номер пакета (1-based), 7 байт данных дальше;
- последний пакет несёт `totalLen % 7` байт (или 7, если без остатка);
- `onTpCm` для BAM: control=32, `totalLen = data[1] | data[2]<<8`,
  `packetsRemaining = data[3]`, PGN = `data[5] | data[6]<<8 | data[7]<<16`
  (сверить порядок с `TPStartWriting`: там `pgn = data[7]<<16 | data[6]<<8 | data[5]`).

## `SnapshotAccumulator.h` / `.cpp`

Карта `(SA,PGN) → {data, len, lastTs, periodMs}`:

```cpp
#pragma once
#include <cstdint>
#include "J1939Proto.h"

// Динамическая карта активных PGN. Ключ — (sa, pgn).
class SnapshotAccumulator {
public:
    static constexpr size_t kMaxRecords = 128;   // AppConfig.maxTrackedPgns

    struct Record {
        uint32_t pgn = 0;
        uint8_t  sa = 0;
        uint16_t len = 0;
        uint8_t  data[J1939Proto::kJ1939MaxDataLen];
        uint32_t lastTsMs = 0;   // монотонный тик последнего обновления
        uint16_t periodMs = 0;   // наблюдаемый период (разница тиков)
        bool     valid = false;
    };

    void update(uint32_t sa, uint32_t pgn,
                const uint8_t* data, size_t len, uint32_t nowMsExMs);

    // Выгрузить «активные» записи (обновлявшиеся не дольше ttlMs назад) в out.
    size_t collect(const Record*& out, uint32_t nowMs, uint32_t ttlMs) const;

    size_t count() const { return count_; }

private:
    Record records_[kMaxRecords];
    size_t count_ = 0;
    size_t findOrInsert(uint32_t sa, uint32_t pgn);   // -1 = карта полна
};
```

Логика `update`:
- найти запись по (sa,pgn); если есть — `periodMs = now - lastTs` (если lastTs!=0),
  скопировать данные, `lastTs = now`;
- если нет — вставить в свободный слот (или перезаписать самую старую по lastTs);
- **конкурентность**: задачу `J1939System` делаем единственным писателем/читателем
  (см. ниже), поэтому мьютекс не нужен. Если позже добавится второй читатель —
  обернуть в `portMUX`/FreeRTOS mutex.

## `J1939System.h` / `.cpp`

```cpp
#pragma once
#include "AppContext.h"
#include "twai/TwaiDriver.h"
#include "J1939Decoder.h"
#include "J1939TransportProtocol.h"
#include "SnapshotAccumulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Координатор J1939: владеет драйвером TWAI, декодером, TP и аккумулятором.
class J1939System {
public:
    explicit J1939System(AppContext* ctx);
    ~J1939System();

    esp_err_t begin();

private:
    AppContext* ctx_;
    TwaiDriver twai_;
    J1939TransportProtocol tp_;
    SnapshotAccumulator acc_;
    TaskHandle_t task_ = nullptr;

    // Обработчик команды клиента «запросить PGN».
    void onJ1939Request(const j1939_request_t* req);

    static void taskWrapper(void* p);
    void taskLoop();

    void processFrame(const TwaiDriver::RxFrame& frame);
    void processAssembled(const J1939AssembledMsg& msg, uint32_t nowMs);
    void sendSnapshot(uint32_t nowMs);
};
```

`begin()`:
- `twai_.begin({...})` (пины/битрейт из `HardwareConfig.h`);
- подписка `J1939_SNAPSHOT...` нет — публикуем сами; подписка
  `ctx_->events.subscribe(APP_EVENTS_BASE, app_event_id_t::J1939_REQUEST,
  &J1939System::onJ1939Request, this)`;
- `xTaskCreatePinnedToCore(taskWrapper, "j1939", 4096, this, 8, &task_, 1)`.

`taskLoop()` — **единственный** владелец аккумулятора:
```
uint32_t nextSnapshot = xTaskGetTickCount();
while (1) {
    uint32_t now = xTaskGetTickCount();
    uint32_t waitMs = (nextSnapshot > now) ? (nextSnapshot - now) : 1;
    TwaiDriver::RxFrame* frame = nullptr;
    if (xQueueReceive(twai_.rxReadyQueue(), &frame, pdMS_TO_TICKS(waitMs)) == pdTRUE) {
        processFrame(*frame);
        xQueueSend(twai_.rxFreeQueue(), &frame, 0);   // вернуть слот
    }
    now = xTaskGetTickCount();
    if ((int32_t)(now - nextSnapshot) >= 0) {         // тик-безопасное сравнение
        sendSnapshot(now);
        nextSnapshot = now + ctx_->config.snapshotIntervalMs;
    }
}
```

`processFrame`:
- `J1939Decoder::decode(frame)` → `J1939PgnMsg`;
- если `pgn == kPgnTpCm` → `tp_.onTpCm`;
- если `pgn == kPgnTpDt` → `if (tp_.onTpDt(msg, out)) processAssembled(out, now)`;
- иначе — одиночный кадр: `acc_.update(sa, pgn, data, dlc, now)`.

`processAssembled` — `acc_.update(sa, pgn, data, len, now)`.

`sendSnapshot(nowMs)`:
- `acc_.collect(records, nowMs, ctx_->config.snapshotTtlMs)` → массив `Record`;
- собрать массив `J1939Proto::BatchRecord` (без копий данных — ссылки на `data`);
- посчитать размер, `malloc` буфер, `J1939Proto::serializeBatch`;
- опубликовать `j1939_snapshot_t` через `postSized(J1939_SNAPSHOT_SEND, buf, size)`:
  размер = `sizeof(j1939_snapshot_t) + payloadLen`; затем `free(buf)`;
- если записей нет — слать пустой батч (count=0) **не нужно**, пропустить.

`onJ1939Request` — построить RQST (порт `requestPGN` из `esp_j1939/j1939twai.cpp`):
- буфер 3 байта: pgn little-endian (`buf[0]=pgn&0xFF; buf[1]=(pgn>>8)&0xFF; buf[2]=(pgn>>16)&0xFF`);
- ID: `priority=6 (0x18), pgn=59904 (0xEA00), src=0xFF (как в старом коде)` →
  `id = (6<<26) | (59904<<8) | 0xFF`; для RQST не peer-to-peer, dest не маскируется
  (в старом коде RQST передавался с `nDestAddr=from` и маской P2P — сверить,
  поведение сохранить);
- `twai_.transmit(id, buf, 3)`.

## Критерий готовности

1. `idf.py reconfigure && idf.py build` — успех.
2. Компонент компилируется (пока не подключён в main).
3. Рекомендуется временно завести `esp_err_t J1939System::begin()` с логированием
   и проверить запуск вручную из консоли — но проще дождаться STEP-07.

## Замечания

- `J1939System` пока не регистрируется в BootManager — это в STEP-07.
- Если после STEP-07 TWAI не поднимется — проверить пины/битрейт и порядок
  `twai_new_node_onchip` + `twai_node_register_event_callbacks` ДО `enable`.
- Не удалять старый `esp_j1939`.