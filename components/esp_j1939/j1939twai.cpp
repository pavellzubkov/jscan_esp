#include <j1939twai.h>

#define CAN_TAG "can_twai"
#define J1939_MY_ADR 25

namespace J1939Twai
{

    namespace
    {
        // J1939Class J1939;
        AppState *_state;
        static const twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();
        static const twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
        twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_GPIO_NUM, CAN_RX_GPIO_NUM, TWAI_MODE_NORMAL);
        static J1939TransportMsgBuf _jmsgbuf[MYJ_BUFFERS_LENGTH];
        static J1939MsgShort mesBuf[MYJ_BUFFERS_SHORT_LENGTH];
        static J1939Msg msgBigBuf[MYJ_BUFFERS_BIG_LENGTH];
        TickType_t moduleStatusTick;

        void resetTwai(void)
        {
            twai_clear_transmit_queue();
            twai_clear_receive_queue();
            twai_stop();
            twai_start();
            twai_initiate_recovery();
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
            twai_message_t tx_msg;
            // Declarations
            uint32_t lID = ((uint32_t)nPriority << 26) + (lPGN << 8) + (uint32_t)nSrcAddr;

            // If PGN represents a peer-to-peer, add destination address to the ID
            if (j1939PeerToPeer(lPGN) == true)
            {
                lID = lID & 0xFFFF00FF;
                lID = lID | ((uint32_t)nDestAddr << 8);

            } // end if

            tx_msg.extd = 1;
            tx_msg.identifier = lID;
            tx_msg.data_length_code = length;
            memcpy(tx_msg.data, nData, length);

            return twai_transmit(&tx_msg, 0);

        } // end j1939Transmit

        // ------------------------------------------------------------------------
        // J1939 Receive
        // ------------------------------------------------------------------------
        J1939MsgShort twaiToj1939Short(twai_message_t *rx_msg)
        {
            J1939MsgShort mes;

            long lPriority = rx_msg->identifier & 0x1C000000;
            mes.nPriority = (int)(lPriority >> 26);

            mes.lPGN = rx_msg->identifier & 0x00FFFF00;
            mes.lPGN = mes.lPGN >> 8;

            rx_msg->identifier = rx_msg->identifier & 0x000000FF;
            mes.nSrcAddr = (int)rx_msg->identifier;

            if (j1939PeerToPeer(mes.lPGN))
            {
                mes.nDestAddr = (int)(mes.lPGN & 0xFF);
                mes.lPGN = mes.lPGN & 0x01FF00;
            }
            mes.nDataLen = 8;
            memcpy(mes.nData, rx_msg->data, rx_msg->data_length_code);

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
            twai_message_t rx_msg;
            uint8_t mesBufindex = 0;
            uint8_t mesBufBigindex = 0;
            while (1)
            {

                esp_err_t er = twai_receive(&rx_msg, portMAX_DELAY); // portMAX_DELAY pdTICKS_TO_MS(20)
                if (er == ESP_OK)
                {
                    if (rx_msg.extd && rx_msg.data_length_code == 8)
                    {
                        moduleStatusTick = xTaskGetTickCount();

                        J1939MsgShort mes = twaiToj1939Short(&rx_msg);

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
                }
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
        // Install TWAI driver
        g_config.intr_flags = ESP_INTR_FLAG_IRAM;
        ESP_ERROR_CHECK(twai_driver_install(&g_config, &t_config, &f_config));
        ESP_LOGW(CAN_TAG, "Driver installed");

        ESP_ERROR_CHECK(twai_start());
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