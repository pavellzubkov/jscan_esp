#pragma once
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_twai_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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
    // до timeout (0 = не ждать). Колбэк/узел уже создан в begin.
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

private:
    // ISR-колбэк приёма (см. .cpp): берёт слот из пула, заполняет и отдаёт в ready.
    static IRAM_ATTR bool rxDoneCb(twai_node_handle_t node,
                                   const twai_rx_done_event_data_t* edata,
                                   void* user_ctx);

    struct RxSlot;    // полное определение — в .cpp

    twai_node_base* node_          = nullptr;
    QueueHandle_t   rxReadyQueue_  = nullptr;
    QueueHandle_t   rxFreeQueue_   = nullptr;
    Config          cfg_           = {};
    RxSlot*         slots_         = nullptr;
};
