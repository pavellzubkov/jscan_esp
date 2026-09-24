# STEP-01 — Скелет слоёв + ядро `01_core/common`

> Статус: **done**

> Контекст: свежая сессия. Проект jscan_esp, целевая архитектура — в
> `docs/PLAN/00-OVERVIEW.md`. Этот шаг добавляет слоистую структуру и ядро,
> **не ломая** текущую сборку (старые плоские компоненты продолжают работать).
> Проверка в конце: `idf.py reconfigure && idf.py build` — старый проект обязан
> собраться, новый core — скомпилироваться (даже если пока никем не используется).

## Цель

1. Корневой `CMakeLists.txt` учит ESP-IDF видеть нумерованные слои компонентов.
2. Создан компонент `components/01_core/common` — ядро:
   `AppContext`, `AppEvents`, `EventManager`, `BootManager`, `SystemTiming`,
   `HardwareConfig`, `AppConfig`, `J1939Proto`.
3. Проект собирается как раньше.

## Шаг 1.1 — Корневой CMakeLists.txt

Заменить содержимое `CMakeLists.txt` на (скопировать паттерн из
`E:\Projects\Embedded\ESP32\Temp_pid\TEMP_PID\CMakeLists.txt`):

```cmake
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/components/01_core
    ${CMAKE_CURRENT_LIST_DIR}/components/02_hardware
    ${CMAKE_CURRENT_LIST_DIR}/components/03_systems
    ${CMAKE_CURRENT_LIST_DIR}/components/04_network
    ${CMAKE_CURRENT_LIST_DIR}/components/05_storage
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(JScaner)
```

Примечания:
- Пустые слои (02..05) пока отсутствуют — EXTRA_COMPONENT_DIRS на несуществующие
  пути не ошибка, но лучше создавать каталоги по мере шагов (следующие STEP-ы).
- Старые плоские компоненты (`_common`, `esp_wifi_start`, ...) в `components/`
  остаются и сканируются ESP-IDF как раньше. Директории `01_core` и т.п. без
  собственного CMakeLists ESP-IDF игнорирует (тот же паттерн, что в TEMP_PID).
- `dependencies.lock`/`managed_components` пересоздаются `idf.py reconfigure`.

## Шаг 1.2 — Компонент `components/01_core/common`

Структура:
```
components/01_core/common/
  CMakeLists.txt
  include/
    AppContext.h
    AppEvents.h
    EventManager.h
    BootManager.h
    SystemTiming.h
    HardwareConfig.h
    AppConfig.h
    J1939Proto.h
  src/
    BootManager.cpp
    J1939Proto.cpp
```

### 1.2.1 `CMakeLists.txt`

```cmake
idf_component_register(
    SRCS "src/BootManager.cpp"
         "src/J1939Proto.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_event freertos log
)
```

`freertos` нужен для `AppContext.h` (очередь event loop), `esp_event` — для
event loop/EventManager. Не добавлять лишних зависимостей (без cjson!).

### 1.2.2 `EventManager.h` и `BootManager.h/.cpp`

**Скопировать без изменений** из TEMP_PID:
- `E:\Projects\Embedded\ESP32\Temp_pid\TEMP_PID\components\01_core\common\include\EventManager.h`
- `...\include\BootManager.h`
- `...\src\BootManager.cpp`

### 1.2.3 `SystemTiming.h`

Аналог TEMP_PID, но под J1939-сканер:

```cpp
#pragma once
#include <cstdint>

namespace Timing {
// Период публикации снапшота J1939 (200–500 мс по чекпоинту).
constexpr uint32_t kSnapshotIntervalMs  = 250;
// Не слать записи, которые не обновлялись дольше TTL (активные PGN).
constexpr uint32_t kSnapshotTtlMs       = 2000;
// Таймаут TP-реасемблера (ожидание пакета TP.DT).
constexpr uint32_t kTransportTimeoutMs  = 1000;
// Пауза между проверками в задачах-«пустышках».
constexpr uint32_t kIdleDelayMs         = 10;
}
```

### 1.2.4 `HardwareConfig.h`

Единая точка правды по железу/адресам (заменит макросы из `j1939twai.h`):

```cpp
#pragma once
#include "driver/gpio.h"
#include <cstdint>

namespace Hw {
// Адрес узла в J1939-сети.
constexpr uint8_t  kJ1939MyAddr = 25;
// Пины TWAI.
constexpr gpio_num_t kCanTxGpio = GPIO_NUM_5;
constexpr gpio_num_t kCanRxGpio = GPIO_NUM_4;
// Битрейт J1939 (CAN 2.0B, 250 kbps).
constexpr uint32_t kCanBitrate = 250000;
// PGN служебных сообщений.
constexpr uint32_t kPgnRequest   = 59904;   // RQST
constexpr uint32_t kPgnTpCm      = 60416;   // TP.CM (connection management)
constexpr uint32_t kPgnTpDt      = 60160;   // TP.DT (data transfer)
}
```

### 1.2.5 `AppConfig.h`

Конфигурация приложения (плоская структура с дефолтами, пишет её только
ConfigStore — создаётся в STEP-04):

```cpp
#pragma once
#include <cstdint>

struct AppConfig {
    // Точка доступа
    char     apSsid[32]     = "J1939_AP";
    char     apPassword[32] = "12345678";
    uint8_t  apChannel      = 12;
    uint8_t  maxStaConn     = 2;
    // Снапшот J1939
    uint32_t snapshotIntervalMs = 250;   // период батч-фрейма (200–500 мс)
    uint32_t snapshotTtlMs       = 2000; // TTL «активности» PGN
    uint16_t maxTrackedPgns      = 128;  // потолок карты аккумулятора
};
```

### 1.2.6 `AppContext.h`

Упрощённая версия TEMP_PID: без adata/FieldRegistry/ConfigStoreIf. Только
event loop + EventManager + конфиг.

```cpp
#pragma once
#include "AppEvents.h"
#include "AppConfig.h"
#include "EventManager.h"
#include "esp_event.h"

struct AppContext {
    esp_event_loop_handle_t event_loop = nullptr;
    EventManager events;
    AppConfig config;   // пишет только ConfigStore

    AppContext() = default;
    ~AppContext() {
        events.shutdown();            // снять подписки ДО удаления loop
        if (event_loop) esp_event_loop_delete(event_loop);
    }
    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;

    esp_err_t initEventLoop() {
        esp_event_loop_args_t args = {
            .queue_size = 256,
            .task_name = "app_event_loop",
            .task_priority = 5,
            .task_stack_size = 4096,
            .task_core_id = 1};
        esp_err_t err = esp_event_loop_create(&args, &event_loop);
        if (err != ESP_OK) return err;
        events.setEventLoop(event_loop);
        return ESP_OK;
    }
};
```

### 1.2.7 `AppEvents.h`

События + структуры данных (образец — TEMP_PID `AppEvents.h`, но без PID-мусора):

```cpp
#pragma once
#include "esp_event_base.h"
#include <cstddef>
#include <cstdint>

ESP_EVENT_DEFINE_BASE(APP_EVENTS_BASE);

enum class app_event_id_t : int32_t {
    J1939_SNAPSHOT_SEND,    // данные: j1939_snapshot_t (postSized), байты батча
    J1939_REQUEST,          // данные: j1939_request_t — команда «послать RQST»
    WS_MESSAGE_RECEIVED,    // данные: ws_message_t (входящее WS-сообщение)
    WS_MESSAGE_SEND,        // данные: ws_message_t (исходящее WS-сообщение)
    WS_CLIENT_CONNECTED,    // данные: ws_message_t (sockfd только)
    WS_CLIENT_DISCONNECTED, // данные: ws_message_t (sockfd только)
    WIFI_STATUS,            // данные: wifi_status_event_t
    CONFIG_CHANGED          // данные: config_changed_event_t (задел)
};

// Максимальная длина WS-сообщения (батч снапшота до 8 КБ).
constexpr size_t kMaxWsMessageLen = 8192;

struct ws_message_t {
    int  sockfd;   // -1 = broadcast
    size_t length;
    char data[];   // flexible array
};

// Байты батча снапшота от J1939System → CommunicationModule (postSized).
struct j1939_snapshot_t {
    size_t length;
    uint8_t data[];
};

// Команда клиента: запросить PGN по J1939 (RQST, PGN 59904).
struct j1939_request_t {
    uint8_t  dstAddr;   // адрес назначения (адрес запрашиваемого узла)
    uint32_t pgn;       // запрашиваемый PGN (напр. 65227/65228)
};

struct wifi_status_event_t {
    bool    is_ap_mode;
    bool    is_connected;
    uint8_t num_clients;
    int8_t  rssi;
    uint8_t disconnect_reason;
};

struct config_changed_event_t {
    int field;   // индекс поля в AppConfig (перечислить в AppConfig.h при надобности)
};
```

### 1.2.8 `J1939Proto.h` / `J1939Proto.cpp`

Чистые функции протокола (без зависимостей от AppContext/железа). Формат — в
`docs/PROTOCOL-J1939.md`.

```cpp
#pragma once
#include <cstddef>
#include <cstdint>

// Формат кадра и батча — см. docs/PROTOCOL-J1939.md
namespace J1939Proto {

constexpr uint8_t  kMagic0        = 0x5A;
constexpr uint8_t  kMagic1        = 0xA5;
constexpr uint8_t  kVersion       = 1;
constexpr uint16_t kHeaderSize    = 10;   // magic(2)+ver(1)+flags(1)+msgType(2)+len(2)+seq(2)
constexpr uint16_t kCrcSize       = 2;
constexpr uint16_t kMinPacketSize = kHeaderSize + kCrcSize;
constexpr uint8_t  kFlagSnapshot  = 0x20;
constexpr uint16_t kMsgTypeSnapshot = 0x0001;
constexpr uint16_t kMsgTypeRequest  = 0x0002;
constexpr uint16_t kJ1939MaxDataLen = 1785;  // 255 пакетов TP.DT * 7

// Запись батча (один PGN).
struct BatchRecord {
    uint8_t       sa;
    uint32_t      pgn;   // 24 бита (0..0x3FFFF)
    uint16_t      len;   // 1..1785
    const uint8_t* data;
    uint16_t      periodMs;
};

uint16_t crc16(const uint8_t* data, size_t len);       // CCITT-FALSE, init 0xFFFF
size_t   batchPayloadSize(const BatchRecord* recs, size_t count); // нужный размер
size_t   serializeBatch(uint8_t* out, size_t outCap,
                        const BatchRecord* recs, size_t count);   // 0 = не влезло
size_t   wrapFrame(uint16_t msgType, uint8_t flags,
                   const uint8_t* payload, size_t payloadLen,
                   uint16_t seq, uint8_t* out, size_t outCap);    // 0 = не влезло
bool     unwrapFrame(const uint8_t* frame, size_t len,
                     uint16_t* msgType, uint8_t* flags,
                     const uint8_t** payload, size_t* payloadLen); // + проверка CRC
}
```

`J1939Proto.cpp`:
- таблица CRC16 — **скопировать** из TEMP_PID `communication/src/CommModule.cpp`
  (первые ~45 строк: `CRC16_TABLE` + функция `crc16`);
- `serializeBatch`: `out[0]=count`, затем на каждую запись: `sa`, `pgn` (LE 3 байта),
  `len`, `data`, `periodMs` (LE 2 байта);
- `wrapFrame`: магия, ver, flags, msgType LE, payloadLen LE, seq LE, payload, CRC16 LE;
- `unwrapFrame`: проверка magic/ver/CRC и заполнение полей (для входящих команд).

## Критерий готовности

1. `idf.py reconfigure` проходит без ошибок (пересоздаёт `build/compile_commands.json`).
2. `idf.py build` — успех (старый проект + новый неиспользуемый `common`).
3. clangd продолжает работать (`.clangd` уже убирает `-f*`/`-m*`).

## Замечания для следующего контекста

- `docs/PROTOCOL-J1939.md` и `docs/PLAN/*` уже созданы отдельно (см. коммит планов).
- Новый core пока никто не использует — это нормально до STEP-07.
- Не удалять старые компоненты и старый `main.cpp` в этом шаге.