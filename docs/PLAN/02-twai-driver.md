# STEP-02 — `02_hardware/twai` (TwaiDriver)

> Контекст: свежая сессия. STEP-01 выполнен (`01_core/common` существует,
> проект собирается). Этот шаг выносит TWAI-часть из старого
> `components/esp_j1939/j1939twai.cpp` в «глупый» драйвер без AppContext.
> Проверка: `idf.py reconfigure && idf.py build`.

## Цель

Создать `components/02_hardware/twai` — самодостаточный драйвер TWAI:
создание узла (ESP-IDF 6.1, onchip API), ISR-слоты приёма с очередями,
передача кадров, обработка bus-off. **Без** декодирования J1939, без TP,
без задач приложения — это делает J1939System (STEP-03).

## Структура

```
components/02_hardware/twai/
  CMakeLists.txt
  include/TwaiDriver.h
  src/TwaiDriver.cpp
```

## `include/TwaiDriver.h`

```cpp
#pragma once
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <cstdint>

// «Глупый» драйвер TWAI: без AppContext, без событий, без задач приложения.
// Приём: ISR-колбэк кладёт кадры в пул слотов и шлёт указатели в очередь готовых.
// Чтение — из очереди готовых (rxReadyQueue), возврат слота — в rxFreeQueue.
class TwaiDriver {
public:
    struct Config {
        gpio_num_t tx = GPIO_NUM_5;
        gpio_num_t rx = GPIO_NUM_4;
        uint32_t   bitrate      = 250000;
        uint8_t    txQueueDepth = 4;
        uint8_t    rxSlots      = 8;      // размер пула ISR-слотов
        int        intrPriority = 1;
    };

    struct RxFrame {
        uint32_t id;     // 29-бит CAN ID (extended)
        uint8_t  dlc;    // 0..8
        uint8_t  data[8];
    };

    esp_err_t begin(const Config& cfg);
    void      end();

    // Передача extended-кадра. Не блокирует навсегда: ждёт окончания передачи
    // до timeout мс (0 = не ждать). Колбэк/узел уже создан в begin.
    esp_err_t transmit(uint32_t id, const uint8_t* data, uint8_t dlc,
                       TickType_t timeout = pdMS_TO_TICKS(100));

    // Очередь указателей на RxFrame (получатель обязан вернуть слот через
    // rxFreeQueue после чтения данных).
    QueueHandle_t rxReadyQueue() const;
    QueueHandle_t rxFreeQueue() const;

    bool started() const { return node_ != nullptr; }

private:
    twai_node_handle_t node_ = nullptr;    // тип из esp_twai.h (см. cpp)
    QueueHandle_t rxReadyQueue_ = nullptr;
    QueueHandle_t rxFreeQueue_  = nullptr;
    // ... внутренний пул слотов, ISR-колбэк (детали в cpp)
};
```

## `src/TwaiDriver.cpp` — что перенести из старого кода

Источник: `components/esp_j1939/j1939twai.cpp` (строки ~24–65, 310–363).

1. **Пул слотов + ISR-колбэк** — скопировать как есть:
   - структура `J1939RxSlot { twai_frame_t frame; uint8_t data[TWAI_FRAME_MAX_LEN]; }`
     → переименовать в приватную `RxSlot` (внутри класса или в анонимном namespace);
   - `rx_done_cb` → статический член класса: берёт слот из `rxFreeQueue_`,
     заполняет, отдаёт в `rxReadyQueue_` (те же `xQueueReceiveFromISR`/
     `xQueueSendFromISR`), возвращает `hpw`.
2. **begin()** — перенести из `J1939Twai::Run` (часть до создания задач):
   - создать очереди `rxReadyQueue_`/`rxFreeQueue_` (глубина `rxSlots`);
   - заполнить пул слотов в `rxFreeQueue_`;
   - `twai_onchip_node_config_t node_config = {};` с полями из `Config`;
   - `twai_new_node_onchip(&node_config, &node_)`;
   - `twai_event_callbacks_t cbs = {}; cbs.on_rx_done = rxDoneCb;`
     `twai_node_register_event_callbacks(node_, &cbs, this);`
   - `twai_node_enable(node_)`.
3. **transmit()** — из `j1939ToTwai` (собрать `twai_frame_t`, extended ide=1,
   `twai_node_transmit` + `twai_node_transmit_wait_all_done(node_, timeout)`).
   ВАЖНО: старый код передавал через `static`-буферы — в новом драйвере для
   потокобезопасности использовать локальные буферы (стек) или небольшой mutex;
   передача с `wait_all_done` делает фрейм безопасным после выхода.
4. **end()** — `twai_node_disable(node_)` + `twai_node_delete(node_)`.
5. **reset/recover (bus-off)** — оставить метод `recover()` (порт `resetTwai`),
   вызывать при необходимости из J1939System; в этом шаге можно не вызывать.

## `CMakeLists.txt`

```cmake
idf_component_register(
    SRC_DIRS .
    INCLUDE_DIRS .
    REQUIRES driver esp_driver_twai freertos
)
```

## Критерий готовности

1. `idf.py reconfigure` без ошибок.
2. `idf.py build` — успех (новый `twai` компилируется; старый `esp_j1939`
   не тронут и продолжает работать).
3. При желании — временный вызов из старого `main.cpp` не делаем: драйвер
   проверяется в STEP-03 (J1939System).

## Замечания

- Тип `twai_node_handle_t` и `twai_frame_t` требуют `#include "esp_twai.h"` /
  `"esp_twai_onchip.h"` в cpp (в заголовке — forward-declare через `void*`
  или включить только в cpp, чтобы не тащить driver в заголовок).
- Не удалять старый `esp_j1939` — он живёт до STEP-07.