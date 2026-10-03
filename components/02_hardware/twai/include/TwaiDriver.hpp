#pragma once
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_twai_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <atomic>
#include <cstdint>

// «Глупый» драйвер TWAI: без AppContext, без событий, без задач приложения.
// Приём: ISR-колбэк кладёт кадры в пул слотов и шлёт указатели в очередь готовых.
// Чтение — из очереди готовых (rxReadyQueue), возврат слота — в rxFreeQueue.
// Полный тип слота (RxSlot) и вызовы API — только в .cpp (esp_twai.h/esp_twai_onchip.h).
class TwaiDriver {
public:
    struct Config {
        gpio_num_t tx = GPIO_NUM_5;
        gpio_num_t rx = GPIO_NUM_4;
        uint32_t   bitrate      = 250000;
        uint8_t    txQueueDepth = 4;
        uint8_t    rxSlots      = 16;     // размер пула ISR-слотов (полная шинная
                                           // нагрузка исчерпывает 8 за миллисекунды)
        int        intrPriority = 1;
    };

    struct RxFrame {
        uint32_t id;     // 29-бит CAN ID (extended)
        uint8_t  dlc;    // 0..8
        uint8_t  data[8];
    };

    // Рантайм-состояние шины (для телеметрии J1939System).
    struct Status {
        int      state;   // twai_error_state_t: ACTIVE/WARNING/PASSIVE/BUS_OFF
        uint32_t txErr;   // TX error count
        uint32_t rxErr;   // RX error count
    };

    esp_err_t begin(const Config& cfg);
    void      end();

    // RAII: узел/очереди/мьютекс/пулы освобождаются автоматически.
    // end() идемпотентен (cleanupPartial зануляет хэндлы), поэтому вызов
    // и из dtor, и вручную безопасен. Копирование запрещено: за узлом и
    // ISR-очередями стоит владение одним ресурсом.
    TwaiDriver() = default;
    ~TwaiDriver() { end(); }
    TwaiDriver(const TwaiDriver&) = delete;
    TwaiDriver& operator=(const TwaiDriver&) = delete;

    // Заполняет Status текущим состоянием узла через twai_node_get_info.
    // Возвращает false, если узел не создан (begin не вызывался).
    bool getStatus(Status& out) const;

    // Передача extended-кадра. Сериализована внутренним мьютексом (кадр живёт
    // в пуле TX-блоков драйвера). Не блокирует навсегда: ждёт окончания
    // передачи до timeout (0 = не ждать — ESP_OK значит «кадр поставлен»).
    // Возвращает ESP_ERR_TIMEOUT, если кадр не поставлен (очередь драйвера
    // занята) либо передача не завершилась за timeout (кадр ещё может уйти).
    esp_err_t transmit(uint32_t id, const uint8_t* data, uint8_t dlc,
                       TickType_t timeout = pdMS_TO_TICKS(100));

    // Восстановление после bus-off: disable/enable + recover при необходимости.
    // Вызывается из J1939System по событию ошибки.
    esp_err_t recover();

    // Очередь указателей на RxFrame (получатель обязан вернуть слот через
    // rxFreeQueue после чтения данных).
    QueueHandle_t rxReadyQueue() const;
    QueueHandle_t rxFreeQueue() const;

    bool started() const { return node_ != nullptr; }

    // Дроп-счётчики (32-бит атомики — lock-free на ESP32, безопасно из ISR):
    // rxDrops — в ISR не было свободного слота либо ready-очередь отказалась
    // принять указатель (слот возвращается в free, кадр теряется);
    // txDrops — пул TX-блоков исчерпан и кадр не поставлен в очередь драйвера.
    uint32_t rxDrops() const { return rxDrops_.load(std::memory_order_relaxed); }
    uint32_t txDrops() const { return txDrops_.load(std::memory_order_relaxed); }

private:
    // ISR-колбэк приёма (см. .cpp): берёт слот из пула, заполняет и отдаёт в ready.
    static IRAM_ATTR bool rxDoneCb(twai_node_handle_t node,
                                   const twai_rx_done_event_data_t* edata,
                                   void* user_ctx);

    struct RxSlot;    // полное определение — в .cpp

    // TX-блок: драйвер TWAI НЕ копирует кадр — twai_frame_queue хранит
    // указатель на twai_frame_t (twai_frame_queue.c: «.data = data»), а
    // _node_queue_tx держит frame->buffer до завершения передачи. Поэтому
    // буфер нельзя размещать на стеке: блок живёт в пуле драйвера и
    // освобождается только когда узел гарантированно простаивает.
    struct TxBlock {
        twai_frame_t frame;
        uint8_t      data[TWAI_FRAME_MAX_LEN];
        bool         inUse = false;
    };

    // Освобождение ресурсов, выделенных частично в begin() (см. .cpp):
    // очереди, слоты, TX-пул, мьютекс. Все хэндлы обнуляются — end() остаётся
    // идемпотентным. Вызывать только при node_ == nullptr.
    void cleanupPartial();

    // Слоты пула TX-блоков больше никем не используются (узел простаивает).
    void releaseTxBlocks();

    twai_node_base* node_          = nullptr;
    QueueHandle_t   rxReadyQueue_  = nullptr;
    QueueHandle_t   rxFreeQueue_   = nullptr;
    Config          cfg_           = {};
    RxSlot*         slots_         = nullptr;
    TxBlock*        txPool_        = nullptr;
    uint16_t        txPoolSize_    = 0;
    SemaphoreHandle_t txMux_       = nullptr;   // сериализация transmit()/end()
    std::atomic<uint32_t> rxDrops_ {0};         // потерянные RX-кадры (см. rxDrops())
    std::atomic<uint32_t> txDrops_ {0};         // потерянные TX-кадры (см. txDrops())
};
