#include "TwaiDriver.hpp"

#include "RaiiGuards.hpp"
#include "esp_log.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include <cstring>
#include <new>

static const char* TAG = "TwaiDriver";

// Слот приёма: буфер данных + фрейм, заполняемый в ISR-колбэке.
// Определение вложенного типа (объявлен в TwaiDriver.hpp как RxSlot).
struct TwaiDriver::RxSlot {
    twai_frame_t frame;
    uint8_t      data[TWAI_FRAME_MAX_LEN];
};

// ISR-колбэк приёма: вызывается драйвером на каждый принятый кадр.
bool TwaiDriver::rxDoneCb(twai_node_handle_t node,
                          const twai_rx_done_event_data_t* /*edata*/,
                          void* user_ctx)
{
    TwaiDriver* self = static_cast<TwaiDriver*>(user_ctx);
    RxSlot* slot = nullptr;
    BaseType_t hpw = pdFALSE;
    if (xQueueReceiveFromISR(self->rxFreeQueue_, &slot, &hpw) == pdTRUE)
    {
        slot->frame.buffer = slot->data;
        slot->frame.buffer_len = sizeof(slot->data);
        if (twai_node_receive_from_isr(node, &slot->frame) == ESP_OK)
        {
            // Консервация: free+ready+потребитель = rxSlots = глубина ready,
            // поэтому переполнение ready практически невозможно — но проверка
            // дёшева и спасает слот от «зависания» между очередями при будущих
            // правках. При отказе слот возвращается в free, кадр считается дропом.
            if (xQueueSendFromISR(self->rxReadyQueue_, &slot, &hpw) != pdTRUE)
            {
                xQueueSendFromISR(self->rxFreeQueue_, &slot, &hpw);
                self->rxDrops_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        else
        {
            xQueueSendFromISR(self->rxFreeQueue_, &slot, &hpw);
        }
    }
    else
    {
        // Пул слотов исчерпан — кадр молча терялся; теперь считаем.
        self->rxDrops_.fetch_add(1, std::memory_order_relaxed);
    }
    return (hpw == pdTRUE);
}

// Частичная очистка: вызывается на каждом error-path begin() и в end().
// node_ на момент вызова должен быть nullptr — узел удаляется отдельно.
// После очистки все хэндлы обнулены, поэтому end() идемпотентен.
void TwaiDriver::cleanupPartial()
{
    delete[] slots_;
    slots_ = nullptr;

    if (rxFreeQueue_ != nullptr)
    {
        vQueueDelete(rxFreeQueue_);
        rxFreeQueue_ = nullptr;
    }
    if (rxReadyQueue_ != nullptr)
    {
        vQueueDelete(rxReadyQueue_);
        rxReadyQueue_ = nullptr;
    }

    delete[] txPool_;
    txPool_ = nullptr;
    txPoolSize_ = 0;

    if (txMux_ != nullptr)
    {
        vSemaphoreDelete(txMux_);
        txMux_ = nullptr;
    }
}

// Узел простаивает (hw_busy == 0 и очередь TX пуста) — значит драйвер больше
// не держит указатели ни на один TX-блок: все можно вернуть в пул.
void TwaiDriver::releaseTxBlocks()
{
    if (txPool_ == nullptr)
        return;
    for (uint16_t i = 0; i < txPoolSize_; i++)
        txPool_[i].inUse = false;
}

esp_err_t TwaiDriver::begin(const Config& cfg)
{
    if (node_ != nullptr)
        return ESP_ERR_INVALID_STATE;

    cfg_ = cfg;

    // Сериализует transmit()/end(): TX-пул и узел — общие ресурсы
    txMux_ = xSemaphoreCreateMutex();
    if (txMux_ == nullptr)
        return ESP_ERR_NO_MEM;

    // Пул слотов для передачи кадров из ISR в задачу
    rxFreeQueue_ = xQueueCreate(cfg_.rxSlots, sizeof(RxSlot*));
    rxReadyQueue_ = xQueueCreate(cfg_.rxSlots, sizeof(RxSlot*));
    if (rxFreeQueue_ == nullptr || rxReadyQueue_ == nullptr)
    {
        cleanupPartial();
        return ESP_ERR_NO_MEM;
    }

    slots_ = new (std::nothrow) RxSlot[cfg_.rxSlots];
    if (slots_ == nullptr)
    {
        cleanupPartial();
        return ESP_ERR_NO_MEM;
    }

    // Пул TX-блоков: драйвер удерживает указатель на кадр до завершения
    // передачи (даже дольше — при таймауте wait_all_done). Размер на два
    // блока больше очереди драйвера (узел + txQueueDepth), чтобы новый кадр
    // всегда имел свободный блок, пока старые ещё в полёте.
    txPoolSize_ = static_cast<uint16_t>(cfg_.txQueueDepth) + 2;
    txPool_ = new (std::nothrow) TxBlock[txPoolSize_];
    if (txPool_ == nullptr)
    {
        cleanupPartial();
        return ESP_ERR_NO_MEM;
    }

    for (uint8_t i = 0; i < cfg_.rxSlots; i++)
    {
        slots_[i].frame.buffer = slots_[i].data;
        slots_[i].frame.buffer_len = sizeof(slots_[i].data);
        RxSlot* s = &slots_[i];
        xQueueSend(rxFreeQueue_, &s, 0);
    }

    // Новый драйвер TWAI: создаём узел вместо twai_driver_install + twai_start
    twai_onchip_node_config_t node_config = {};
    node_config.io_cfg.tx = cfg_.tx;
    node_config.io_cfg.rx = cfg_.rx;
    node_config.io_cfg.quanta_clk_out = GPIO_NUM_NC;
    node_config.io_cfg.bus_off_indicator = GPIO_NUM_NC;
    node_config.bit_timing.bitrate = cfg_.bitrate;
    node_config.tx_queue_depth = cfg_.txQueueDepth;
    node_config.fail_retry_cnt = -1;    // ретрансляция до успеха, как в старом драйвере
    node_config.intr_priority = cfg_.intrPriority;

    esp_err_t err = twai_new_node_onchip(&node_config, &node_);
    if (err != ESP_OK)
    {
        cleanupPartial();
        return err;
    }

    // Колбэки регистрируются до запуска узла (узел в состоянии stopped)
    twai_event_callbacks_t cbs = {};
    cbs.on_rx_done = rxDoneCb;
    err = twai_node_register_event_callbacks(node_, &cbs, this);
    if (err != ESP_OK)
    {
        twai_node_delete(node_);
        node_ = nullptr;
        cleanupPartial();
        return err;
    }

    err = twai_node_enable(node_);
    if (err != ESP_OK)
    {
        twai_node_delete(node_);
        node_ = nullptr;
        cleanupPartial();
        return err;
    }

    return ESP_OK;
}

void TwaiDriver::end()
{
    // Мьютекс дожидается transmit(), уже зашедшего в другую задачу; сам же
    // end() должен вызываться ПОСЛЕ полной остановки задач-отправителей —
    // иначе узел удалялся бы под передачей, а убитая в transmit() задача
    // навсегда захватила бы txMux_ (см. ~J1939System, ARCH_FIX2 шаг 12).
    if (txMux_ != nullptr)
        xSemaphoreTake(txMux_, portMAX_DELAY);

    if (node_ != nullptr)
    {
        twai_node_disable(node_);
        twai_node_delete(node_);
        node_ = nullptr;
    }

    if (txMux_ != nullptr)
        xSemaphoreGive(txMux_);

    cleanupPartial();
}

esp_err_t TwaiDriver::transmit(uint32_t id, const uint8_t* data, uint8_t dlc,
                               TickType_t timeout)
{
    if (node_ == nullptr)
        return ESP_ERR_INVALID_STATE;
    if (dlc > TWAI_FRAME_MAX_LEN || (dlc > 0 && data == nullptr))
        return ESP_ERR_INVALID_ARG;
    if (txMux_ == nullptr || txPool_ == nullptr || txPoolSize_ == 0)
        return ESP_ERR_INVALID_STATE;

    // RAII-гарантия возврата мьютекса на всех путях возврата
    // (общий примитив из RaiiGuards.hpp — как и прежний локальный TxLock)
    LockGuard lock(txMux_);

    // Узел простаивает → ни один TX-блок больше не нужен драйверу
    if (twai_node_transmit_wait_all_done(node_, 0) == ESP_OK)
        releaseTxBlocks();

    // Свободный блок из пула (кадр и буфер данных живут в драйвере)
    TxBlock* blk = nullptr;
    for (uint16_t i = 0; i < txPoolSize_; i++)
    {
        if (!txPool_[i].inUse)
        {
            blk = &txPool_[i];
            break;
        }
    }

    if (blk == nullptr)
    {
        // Пул исчерпан: кадры ещё в полёте — ждём их окончания в пределах
        // отведённого времени, затем возвращаем блоки в пул
        esp_err_t w = twai_node_transmit_wait_all_done(node_, (int)pdTICKS_TO_MS(timeout));
        if (w != ESP_OK)
        {
            ESP_LOGW(TAG, "tx pool busy, frame dropped (id=0x%lX)", (unsigned long)id);
            txDrops_.fetch_add(1, std::memory_order_relaxed);
            return ESP_ERR_TIMEOUT;
        }
        releaseTxBlocks();
        blk = &txPool_[0];
    }

    // Блок занимаем ДО вызова драйвера: он сохраняет указатели на frame/buffer
    blk->inUse = true;
    blk->frame = {};
    blk->frame.header.id = id;
    blk->frame.header.ide = 1;    // extended (29-бит ID)
    blk->frame.header.dlc = dlc;
    blk->frame.buffer = blk->data;
    blk->frame.buffer_len = dlc;
    if (dlc > 0)
        memcpy(blk->data, data, dlc);

    esp_err_t res = twai_node_transmit(node_, &blk->frame, 0);
    if (res != ESP_OK)
    {
        // Кадр не попал в очередь драйвера — блок никем не занят
        blk->inUse = false;
        return res;
    }

    if (timeout == 0)
        return ESP_OK;   // не ждали: блок освободит следующий transmit()/end()

    // Драйвер держит указатели до конца передачи — ждём её окончания.
    // При таймауте блок остаётся занятым (кадр ещё может уйти в шину) и
    // будет возвращён в пул, когда узел простеет.
    res = twai_node_transmit_wait_all_done(node_, (int)pdTICKS_TO_MS(timeout));
    if (res == ESP_OK)
        releaseTxBlocks();
    else
        ESP_LOGW(TAG, "tx wait %s (id=0x%lX, frame may still be in flight)",
                 esp_err_to_name(res), (unsigned long)id);
    return res;
}

esp_err_t TwaiDriver::recover()
{
    if (node_ == nullptr || txMux_ == nullptr)
        return ESP_ERR_INVALID_STATE;

    // Под txMux_: disable/enable удаляет очередь TX — без лока возможна
    // гонка с transmit(), уже зашедшим в другую задачу.
    LockGuard lock(txMux_);

    // Новый драйвер: disable/enable очищает очереди и перезапускает узел
    twai_node_disable(node_);
    twai_node_enable(node_);
    twai_node_status_t status;
    if (twai_node_get_info(node_, &status, nullptr) == ESP_OK &&
        status.state == TWAI_ERROR_BUS_OFF)
    {
        return twai_node_recover(node_);
    }
    return ESP_OK;
}

bool TwaiDriver::getStatus(Status& out) const
{
    if (node_ == nullptr)
        return false;
    twai_node_status_t st;
    if (twai_node_get_info(node_, &st, nullptr) != ESP_OK)
        return false;
    out.state = static_cast<int>(st.state);
    out.txErr = st.tx_error_count;
    out.rxErr = st.rx_error_count;
    return true;
}

QueueHandle_t TwaiDriver::rxReadyQueue() const { return rxReadyQueue_; }
QueueHandle_t TwaiDriver::rxFreeQueue() const { return rxFreeQueue_; }