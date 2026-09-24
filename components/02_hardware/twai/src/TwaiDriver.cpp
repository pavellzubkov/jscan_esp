#include "TwaiDriver.h"

#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include <cstring>

namespace {

// Слот приёма: буфер данных + фрейм, заполняемый в ISR-колбэке.
struct RxSlot {
    twai_frame_t frame;
    uint8_t      data[TWAI_FRAME_MAX_LEN];
};

} // namespace

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
            xQueueSendFromISR(self->rxReadyQueue_, &slot, &hpw);
        }
        else
        {
            xQueueSendFromISR(self->rxFreeQueue_, &slot, &hpw);
        }
    }
    return (hpw == pdTRUE);
}

esp_err_t TwaiDriver::begin(const Config& cfg)
{
    if (node_ != nullptr)
        return ESP_ERR_INVALID_STATE;

    cfg_ = cfg;

    // Пул слотов для передачи кадров из ISR в задачу
    rxFreeQueue_ = xQueueCreate(cfg_.rxSlots, sizeof(RxSlot*));
    rxReadyQueue_ = xQueueCreate(cfg_.rxSlots, sizeof(RxSlot*));
    if (rxFreeQueue_ == nullptr || rxReadyQueue_ == nullptr)
        return ESP_ERR_NO_MEM;

    slots_ = new (std::nothrow) RxSlot[cfg_.rxSlots];
    if (slots_ == nullptr)
        return ESP_ERR_NO_MEM;

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
        return err;

    // Колбэки регистрируются до запуска узла (узел в состоянии stopped)
    twai_event_callbacks_t cbs = {};
    cbs.on_rx_done = rxDoneCb;
    err = twai_node_register_event_callbacks(node_, &cbs, this);
    if (err != ESP_OK)
    {
        twai_node_delete(node_);
        node_ = nullptr;
        return err;
    }

    err = twai_node_enable(node_);
    if (err != ESP_OK)
    {
        twai_node_delete(node_);
        node_ = nullptr;
        return err;
    }

    return ESP_OK;
}

void TwaiDriver::end()
{
    if (node_ != nullptr)
    {
        twai_node_disable(node_);
        twai_node_delete(node_);
        node_ = nullptr;
    }

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
}

esp_err_t TwaiDriver::transmit(uint32_t id, const uint8_t* data, uint8_t dlc,
                               TickType_t timeout)
{
    if (node_ == nullptr)
        return ESP_ERR_INVALID_STATE;
    if (dlc > TWAI_FRAME_MAX_LEN || (dlc > 0 && data == nullptr))
        return ESP_ERR_INVALID_ARG;

    // Локальные буферы на стеке: безопасно для параллельных вызовов, т.к.
    // передача завершается wait_all_done до выхода из функции.
    twai_frame_t tx_frame = {};
    uint8_t txData[TWAI_FRAME_MAX_LEN] = {};
    if (dlc > 0)
        memcpy(txData, data, dlc);

    tx_frame.header.id = id;
    tx_frame.header.ide = 1;    // extended (29-бит ID)
    tx_frame.header.dlc = dlc;
    tx_frame.buffer = txData;
    tx_frame.buffer_len = dlc;

    esp_err_t res = twai_node_transmit(node_, &tx_frame, 0);
    if (res == ESP_OK)
    {
        // Драйвер держит указатель на фрейм до конца передачи, ждём завершения
        twai_node_transmit_wait_all_done(node_, (int)pdTICKS_TO_MS(timeout));
    }
    return res;
}

esp_err_t TwaiDriver::recover()
{
    if (node_ == nullptr)
        return ESP_ERR_INVALID_STATE;

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

QueueHandle_t TwaiDriver::rxReadyQueue() const { return rxReadyQueue_; }
QueueHandle_t TwaiDriver::rxFreeQueue() const { return rxFreeQueue_; }