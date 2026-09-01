#include <j1939twai.h>
#include <esp_twai.h>
#include <esp_twai_onchip.h>
#include <esp_attr.h>
#include <freertos/queue.h>

#define CAN_TAG "can_twai"

#define MYJ_RX_SLOTS 8

namespace J1939Twai
{

    namespace
    {
        // J1939Class J1939;
        AppState *_state;
        twai_node_handle_t _node_hdl;
        static J1939TransportMsgBuf _jmsgbuf[MYJ_BUFFERS_LENGTH];
        static J1939MsgShort mesBuf[MYJ_BUFFERS_SHORT_LENGTH];
        static J1939Msg msgBigBuf[MYJ_BUFFERS_BIG_LENGTH];
        TickType_t moduleStatusTick;

        // Буферы приёма: заполняются в ISR-колбэке, обрабатываются в задаче
        struct J1939RxSlot
        {
            twai_frame_t frame;
            uint8_t data[TWAI_FRAME_MAX_LEN];
        };
        static J1939RxSlot rxSlots[MYJ_RX_SLOTS];
        static QueueHandle_t rxSlotFreeQueue;
        static QueueHandle_t rxSlotQueue;

        void resetTwai(void)
        {
            // Новый драйвер: disable/enable очищает очереди и перезапускает узел
            twai_node_disable(_node_hdl);
            twai_node_enable(_node_hdl);
            twai_node_status_t status;
            if (twai_node_get_info(_node_hdl, &status, NULL) == ESP_OK && status.state == TWAI_ERROR_BUS_OFF)
            {
                twai_node_recover(_node_hdl);
            }
        }

        // ISR-колбэк приёма: вызывается драйвером на каждый принятый кадр
        static IRAM_ATTR bool rx_done_cb(twai_node_handle_t node, const twai_rx_done_event_data_t *edata, void *user_ctx)
        {
            J1939RxSlot *slot = NULL;
            BaseType_t hpw = pdFALSE;
            if (xQueueReceiveFromISR(rxSlotFreeQueue, &slot, &hpw) == pdTRUE)
            {
                slot->frame.buffer = slot->data;
                slot->frame.buffer_len = sizeof(slot->data);
                if (twai_node_receive_from_isr(node, &slot->frame) == ESP_OK)
                {
                    xQueueSendFromISR(rxSlotQueue, &slot, &hpw);
                }
                else
                {
                    xQueueSendFromISR(rxSlotFreeQueue, &slot, &hpw);
                }
            }
            return (hpw == pdTRUE);
        }

        bool j1939PeerToPeer(long lPGN)
        {
            // Check the PGN
            if (lPGN > 0 && lPGN <= 0xEFFF)
                return true;

            if (lPGN > 0x10000 && lPGN <= 0x1EFFF)
                return true;

            return false;
        }

        // ------------------------------------------------------------------------
        // J1939 Transmit
        // ------------------------------------------------------------------------
        esp_err_t j1939ToTwai(uint32_t lPGN, uint8_t nPriority, uint8_t nSrcAddr, uint8_t nDestAddr, uint8_t *nData, uint16_t length = 8)
        {
            static twai_frame_t tx_msg;
            static uint8_t txData[TWAI_FRAME_MAX_LEN];
            // Declarations
            uint32_t lID = ((uint32_t)nPriority << 26) + (lPGN << 8) + (uint32_t)nSrcAddr;

            // If PGN represents a peer-to-peer, add destination address to the ID
            if (j1939PeerToPeer(lPGN) == true)
            {
                lID = lID & 0xFFFF00FF;
                lID = lID | ((uint32_t)nDestAddr << 8);

            } // end if

            tx_msg.header.id = lID;
            tx_msg.header.ide = 1;
            tx_msg.header.dlc = length;
            tx_msg.buffer = txData;
            tx_msg.buffer_len = length;
            memcpy(txData, nData, length);

            esp_err_t res = twai_node_transmit(_node_hdl, &tx_msg, 0);
            if (res == ESP_OK)
            {
                // Драйвер держит указатель на фрейм до конца передачи, ждём завершения
                twai_node_transmit_wait_all_done(_node_hdl, 100);
            }
            return res;

        } // end j1939Transmit

        // ------------------------------------------------------------------------
        // J1939 Receive
        // ------------------------------------------------------------------------
        J1939MsgShort twaiToj1939Short(twai_frame_t *rx_msg)
        {
            J1939MsgShort mes;

            uint32_t id = rx_msg->header.id;
            long lPriority = id & 0x1C000000;
            mes.nPriority = (int)(lPriority >> 26);

            mes.lPGN = id & 0x00FFFF00;
            mes.lPGN = mes.lPGN >> 8;

            mes.nSrcAddr = (int)(id & 0x000000FF);

            if (j1939PeerToPeer(mes.lPGN))
            {
                mes.nDestAddr = (int)(mes.lPGN & 0xFF);
                mes.lPGN = mes.lPGN & 0x01FF00;
            }
            mes.nDataLen = 8;
            memcpy(mes.nData, rx_msg->buffer, rx_msg->header.dlc);

            return mes;
        }

        // ============= J1939 transport protocol =========================

        void TPStartWriting(J1939MsgShort mes)
        {
            for (uint8_t i = 0; i < MYJ_BUFFERS_LENGTH; i++)
            {

                if (!_jmsgbuf[i].iswriting)
                {
                    _jmsgbuf[i].iswriting = true;
                    _jmsgbuf[i].startTick = xTaskGetTickCount();
                    _jmsgbuf[i].msg.lPGN = mes.nData[7] << 16;
                    _jmsgbuf[i].msg.lPGN = _jmsgbuf[i].msg.lPGN | mes.nData[6] << 8;
                    _jmsgbuf[i].msg.lPGN = _jmsgbuf[i].msg.lPGN | mes.nData[5];
                    _jmsgbuf[i].msg.nSrcAddr = mes.nSrcAddr;
                    _jmsgbuf[i].msg.nDestAddr = mes.nDestAddr;
                    _jmsgbuf[i].msg.nDataLen = mes.nData[2] << 8 | mes.nData[1];
                    _jmsgbuf[i].packages_length = mes.nData[3];
                    _jmsgbuf[i].expected_package = 1;

                    break;
                }
            }
        }

        int TPWriting(J1939MsgShort mes)
        {
            int out = -1;
            for (uint8_t i = 0; i < MYJ_BUFFERS_LENGTH; i++)
            {
                if (_jmsgbuf[i].iswriting)
                {
                    bool overflow = (xTaskGetTickCount() - _jmsgbuf[i].startTick) < 0;
                    bool timeout = (xTaskGetTickCount() - _jmsgbuf[i].startTick) >= MYJ_TP_TIMEOUT;

                    if (overflow || timeout)
                    {
                        _jmsgbuf[i].iswriting = false;
                        return out;
                    }
                }

                uint8_t packetN = mes.nData[0];
                bool timeForPackage = (xTaskGetTickCount() - _jmsgbuf[i].startTick) > 50;

                if (_jmsgbuf[i].iswriting && _jmsgbuf[i].expected_package == packetN && timeForPackage && _jmsgbuf[i].msg.nSrcAddr == mes.nSrcAddr)
                {

                    _jmsgbuf[i].startTick = xTaskGetTickCount();
                    uint8_t writeBufIndex = (packetN - 1) * 7;
                    memcpy(&_jmsgbuf[i].msg.nData[writeBufIndex], &mes.nData[1], 7);
                    _jmsgbuf[i].expected_package++;

                    _jmsgbuf[i].packages_length--;

                    if (_jmsgbuf[i].packages_length == 0)
                    {
                        _jmsgbuf[i].iswriting = false;
                        out = i;
                        break;
                    }
                }
            }
            return out;
        }

        // ============= J1939 transport protocol END =========================

        static void twai_receive_task(void *arg)
        {
            AppState *_appState = (AppState *)arg;
            uint8_t mesBufindex = 0;
            uint8_t mesBufBigindex = 0;
            while (1)
            {
                J1939RxSlot *slot = NULL;
                // Ждём кадр из ISR-колбэка
                if (xQueueReceive(rxSlotQueue, &slot, portMAX_DELAY) != pdTRUE)
                    continue;

                twai_frame_t *rx_msg = &slot->frame;
                if (rx_msg->header.ide && rx_msg->header.dlc == 8)
                {
                    moduleStatusTick = xTaskGetTickCount();

                    J1939MsgShort mes = twaiToj1939Short(rx_msg);

                    switch (mes.lPGN)
                    {
                        //-- transport protocol implement
                    case MYJ_TPCM_PGN:
                    {
                        // ESP_LOGW(CAN_TAG, "mes !!TPCM-, src- %d, dest- %d tick -%d", mes.nSrcAddr, mes.nDestAddr, xTaskGetTickCount());
                        if (mes.nData[0] == 32 && mes.nDestAddr == 255)
                        {
                            TPStartWriting(mes);
                        }

                        break;
                    }

                    case MYJ_TPDT_PGN:
                    {
                        // ESP_LOGW(CAN_TAG, "mes TPDT-, src- %d, dest- %d tick -%d", mes.nSrcAddr, mes.nDestAddr, xTaskGetTickCount());

                        if (mes.nDestAddr == 255)
                        {
                            int res;
                            res = TPWriting(mes);
                            if (res != -1)
                            {
                                // ESP_LOGW(CAN_TAG, "mes STOP write-, src- %d, bufInd- %d tick -%d", _jmsgbuf[res].msg.nSrcAddr, res, xTaskGetTickCount());
                                // ESP_LOGW(CAN_TAG, "mes STOP t buf -, pgn- %d, dlen- %d tick -%d", _jmsgbuf[res].msg.lPGN, _jmsgbuf[res].msg.nDataLen, xTaskGetTickCount());
                                msgBigBuf[mesBufBigindex] = _jmsgbuf[res].msg;
                                J1939Msg *ms = &msgBigBuf[mesBufBigindex];
                                xQueueSend(_state->j1939module.mesLongQueue, &ms, 0);
                                mesBufBigindex++;
                                if (mesBufBigindex == MYJ_BUFFERS_BIG_LENGTH)
                                    mesBufBigindex = 0;
                                // adr->j1939module.message = _jmsgbuf[res].msg;
                                // ESP_LOGW(CAN_TAG, "mes big pgn- %d, len- %d, tick -%d", _jmsgbuf[res].msg.lPGN, _jmsgbuf[res].msg.nDataLen, xTaskGetTickCount());
                            };
                        }

                        break;
                    }

                    default:
                    {
                        if(mes.lPGN==65226 || mes.lPGN==65227){
                            ESP_LOGW(CAN_TAG, "Short queue send pgn %d from %d",mes.lPGN,mes.nSrcAddr);
                        }
                        mesBuf[mesBufindex] = mes;
                        J1939MsgShort *ms = &mesBuf[mesBufindex];
                        xQueueSend(_state->j1939module.mesShortQueue, &ms, 0);
                        mesBufindex++;
                        if (mesBufindex == MYJ_BUFFERS_SHORT_LENGTH)
                            mesBufindex = 0;
                        break;
                    }
                    }
                }

                // Возвращаем слот в пул для ISR
                xQueueSend(rxSlotFreeQueue, &slot, 0);
            }
            vTaskDelete(NULL);
        }
    }

    static void requestPGN(uint8_t from, uint32_t pgnN)
    {
        ESP_LOGW(CAN_TAG, "Try send Req to ECM %X", pgnN);

        uint8_t buf[8];
        buf[3] = (pgnN & 0xff000000) >> 24;
        buf[2] = (pgnN & 0x00ff0000) >> 16;
        buf[1] = (pgnN & 0x0000ff00) >> 8;
        buf[0] = pgnN & 0x000000ff;

        ESP_LOGW(CAN_TAG, "Try send Req to ECM buf0 %Xx%Xx%Xx%X", buf[0], buf[1], buf[2], buf[3]);

        esp_err_t res = j1939ToTwai(59904, 6, 255, from, buf, 3);
        if (res != ESP_OK)
        {
            ESP_LOGW(CAN_TAG, "Fail send Req to ECM");
        }
    }

    esp_err_t Run(AppState *state)
    {
        _state = state;

        // Пул слотов для передачи кадров из ISR в задачу
        rxSlotFreeQueue = xQueueCreate(MYJ_RX_SLOTS, sizeof(J1939RxSlot *));
        rxSlotQueue = xQueueCreate(MYJ_RX_SLOTS, sizeof(J1939RxSlot *));
        if (rxSlotFreeQueue == NULL || rxSlotQueue == NULL)
            return ESP_FAIL;

        for (uint8_t i = 0; i < MYJ_RX_SLOTS; i++)
        {
            rxSlots[i].frame.buffer = rxSlots[i].data;
            rxSlots[i].frame.buffer_len = sizeof(rxSlots[i].data);
            J1939RxSlot *s = &rxSlots[i];
            xQueueSend(rxSlotFreeQueue, &s, 0);
        }

        // Новый драйвер TWAI: создаём узел вместо twai_driver_install + twai_start
        twai_onchip_node_config_t node_config = {};
        node_config.io_cfg.tx = CAN_TX_GPIO_NUM;
        node_config.io_cfg.rx = CAN_RX_GPIO_NUM;
        node_config.io_cfg.quanta_clk_out = GPIO_NUM_NC;
        node_config.io_cfg.bus_off_indicator = GPIO_NUM_NC;
        node_config.bit_timing.bitrate = 250000;
        node_config.tx_queue_depth = 4;
        node_config.fail_retry_cnt = -1;    // ретрансляция до успеха, как в старом драйвере
        node_config.intr_priority = 1;

        ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &_node_hdl));

        // Колбэки регистрируются до запуска узла (узел в состоянии stopped)
        twai_event_callbacks_t cbs = {};
        cbs.on_rx_done = rx_done_cb;
        ESP_ERROR_CHECK(twai_node_register_event_callbacks(_node_hdl, &cbs, NULL));
        ESP_LOGW(CAN_TAG, "Driver installed");

        ESP_ERROR_CHECK(twai_node_enable(_node_hdl));
        ESP_LOGW(CAN_TAG, "Driver started");

        _state->j1939module.mesShortQueue = xQueueCreate(10, sizeof(J1939MsgShort *));

        if (_state->j1939module.mesShortQueue == NULL)
            return ESP_FAIL;
        _state->j1939module.mesLongQueue = xQueueCreate(3, sizeof(J1939Msg *));

        if (_state->j1939module.mesLongQueue == NULL)
            return ESP_FAIL;

        _state->j1939module.reqFunc = requestPGN;

        xTaskCreatePinnedToCore(twai_receive_task, "TWAI_rx", 4096, state, CAN_RX_TASK_PRIO, NULL, 1);
        return ESP_OK;
    }
}