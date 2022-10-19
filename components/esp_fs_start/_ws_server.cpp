#include <_ws_server.h>

namespace WebSockServer
{
    namespace
    {
        static const char *TAG = "wsserver";
        AppState *state;

        /*
         * Structure holding server handle
         * and internal socket fd in order
         * to use out of request send
         */
        struct async_resp_arg
        {
            httpd_handle_t hd;
            int fd;
        };

        /*
         * async send function, which we put into the httpd work queue
         */
        static void ws_async_send(void *arg)
        {
            static const char *data = "Async data";
            struct async_resp_arg *resp_arg = (async_resp_arg *)arg;
            httpd_handle_t hd = resp_arg->hd;
            int fd = resp_arg->fd;
            httpd_ws_frame_t ws_pkt;
            memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
            ws_pkt.payload = (uint8_t *)data;
            ws_pkt.len = strlen(data);
            ws_pkt.type = HTTPD_WS_TYPE_TEXT;

            httpd_ws_send_frame_async(hd, fd, &ws_pkt);
            free(resp_arg);
        }

        static esp_err_t trigger_async_send(httpd_handle_t handle, httpd_req_t *req)
        {
            struct async_resp_arg *resp_arg = (async_resp_arg *)malloc(sizeof(struct async_resp_arg));
            resp_arg->hd = req->handle;
            resp_arg->fd = httpd_req_to_sockfd(req);
            return httpd_queue_work(handle, ws_async_send, resp_arg);
        }

        static void send_ping(void *arg)
        {
            struct async_resp_arg *resp_arg = (async_resp_arg *)arg;
            httpd_handle_t hd = resp_arg->hd;
            int fd = resp_arg->fd;
            httpd_ws_frame_t ws_pkt;
            memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
            ws_pkt.payload = NULL;
            ws_pkt.len = 0;
            ws_pkt.type = HTTPD_WS_TYPE_PING;

            httpd_ws_send_frame_async(hd, fd, &ws_pkt);
            free(resp_arg);
        }

        /* Simple handler for websocket */
        static esp_err_t wsendpointhandler(httpd_req_t *req)
        {
            if (req->method == HTTP_GET)
            {
                ESP_LOGI(TAG, "Handshake done, the new connection was opened");
                // ESP_LOGI(TAG, "ws %s", req->user_ctx);
                return ESP_OK;
            }

            httpd_ws_frame_t ws_pkt;
            uint8_t *buf = NULL;
            memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
            // ws_pkt.type = HTTPD_WS_TYPE_TEXT;
            /* Set max_len = 0 to get the frame len */
            esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
                return ret;
            }

            // if (ws_pkt.type == HTTPD_WS_TYPE_TEXT && ws_pkt.len == 4)
            // {
            //     return ESP_OK;
            // }

            // ESP_LOGW(TAG, "frame len is %d", ws_pkt.len);
            // ESP_LOGW(TAG, "frame type is %d", ws_pkt.type);

            if (ws_pkt.len)
            {
                /* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
                buf = (uint8_t *)calloc(1, ws_pkt.len + 1);
                uint16_t tyhgf = 0;
                // memcpy(&tyhgf, ws_pkt.payload, 2);
                // tyhgf = (uint16_t)*ws_pkt.payload;
                ws_pkt.payload = buf;
                /* Set max_len = ws_pkt.len to get the frame payload */
                ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
                memcpy(&tyhgf, buf, 2);
                ESP_LOGW(TAG, "payload is %d", tyhgf);
                if (ret != ESP_OK)
                {
                    ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
                    free(buf);
                    return ret;
                }

                if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE)
                {
                    // Response CLOSE packet with no payload to peer
                    ESP_LOGW(TAG, "Get cloce frame");
                    ws_pkt.len = 0;
                    ws_pkt.payload = NULL;
                    return ESP_OK;
                }

                switch (buf[0])
                {
                case MESS_WORK:
                {
                    ESP_LOGW(TAG, "Work Reg Val %d", buf[1]);
                    state->j1939module.workType = (WorkReg)buf[1];
                    // Serial.printf("Work Reg Val : %d \n", payload[1]);
                    // stateAdrwifi->workReg = (long)payload[1];
                    break;
                }

                case MESS_SET_SPEED:
                {
                    break;
                }

                case MESS_SET_GEAR:
                {
                    break;
                }

                case MESS_SEND_1939REQ:
                {
                    ESP_LOGW(TAG, "Try request  ");

                    switch (buf[1])
                    {
                    case GET_STORED_CODES:
                        state->j1939module.reqFunc(buf[2], 65227);
                        break;
                    case CLEAR_STORED_CODES:
                        state->j1939module.reqFunc(buf[2], 65228);
                        break;
                    default:
                        break;
                    }

                    // Serial.printf("Work Reg Val : %d \n", payload[1]);
                    // stateAdrwifi->workReg = (long)payload[1];
                    break;
                }

                default:
                    break;
                }

                // ESP_LOGI(TAG, "req is %d", req->user_ctx);
                free(buf);
            }
            return ret;
        }

        esp_err_t httpd_ws_send_frame_to_all_clients(httpd_ws_frame_t *ws_pkt, bool async = true)
        {
            static constexpr size_t max_clients = CONFIG_LWIP_MAX_LISTENING_TCP;
            size_t fds = max_clients;
            int client_fds[max_clients] = {0};

            esp_err_t ret = httpd_get_client_list(state->sysModule.server, &fds, client_fds);

            if (ret != ESP_OK)
            {
                return ret;
            }

            for (int i = 0; i < fds; i++)
            {
                auto client_info = httpd_ws_get_fd_info(state->sysModule.server, client_fds[i]);

                if (client_info == HTTPD_WS_CLIENT_WEBSOCKET)
                {
                    // httpd_ws_send_frame_async(wsserver, client_fds[i], ws_pkt);

                    esp_err_t ret1 = async ? httpd_ws_send_frame_async(state->sysModule.server, client_fds[i], ws_pkt) : httpd_ws_send_data(state->sysModule.server, client_fds[i], ws_pkt);

                    if (ret1 != ESP_OK)
                    {
                        ESP_LOGW(TAG, "Ws send er - %d", ret1);
                        httpd_sess_trigger_close(state->sysModule.server, client_fds[i]);
                    }
                }
            }

            return ESP_OK;
        }

        static void sendPingMesTask(void *arg)
        {
            while (1)
            {
                httpd_ws_frame_t ws_pkt;
                memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
                ws_pkt.type = HTTPD_WS_TYPE_BINARY;
                ws_pkt.final = true;
                uint16_t bufSize = 2;
                uint8_t *buffer = (uint8_t *)malloc(bufSize);
                buffer[0] = MES_WS_PING;
                ws_pkt.len = bufSize;
                ws_pkt.payload = buffer;
                if (httpd_ws_send_frame_to_all_clients(&ws_pkt, false) != ESP_OK)
                {
                    ESP_LOGE(TAG, "httpd_ws_send_frame failed");
                }
                free(buffer);

                vTaskDelay(500); /// portTICK_RATE_MS
            }
        }

        static void sendJ1939Mes(J1939MsgShort *mes)
        {
            httpd_ws_frame_t ws_pkt;
            memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
            ws_pkt.type = HTTPD_WS_TYPE_BINARY;
            ws_pkt.final = true;
            uint16_t bufSize = sizeof(J1939MsgShort) + 1;
            uint8_t *buffer = (uint8_t *)malloc(bufSize);
            buffer[0] = MES_J1939;
            memcpy(&buffer[1], mes, sizeof(J1939MsgShort));
            ws_pkt.len = bufSize;
            ws_pkt.payload = buffer;
            if (httpd_ws_send_frame_to_all_clients(&ws_pkt, true) != ESP_OK)
            {
                ESP_LOGE(TAG, "httpd_ws_send_frame failed");
            }
            free(buffer);
        }

        static void sendJ1939Mes(J1939Msg *mes)
        {
            httpd_ws_frame_t ws_pkt;
            memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
            ws_pkt.type = HTTPD_WS_TYPE_BINARY;
            ws_pkt.final = true;
            uint16_t payloadSize = mes->nDataLen;
            uint16_t bufSize = payloadSize + 10;
            uint8_t *buffer = (uint8_t *)malloc(bufSize);
            buffer[0] = MES_J1939;
            memcpy(&buffer[1], mes, bufSize - 1);
            ws_pkt.len = bufSize;
            ws_pkt.payload = buffer;

            if (httpd_ws_send_frame_to_all_clients(&ws_pkt, true) != ESP_OK)
            {
                ESP_LOGE(TAG, "httpd_ws_send_frame failed");
            }
            free(buffer);
        }

        void WsSendTask(void *pvParameters)
        {
            AppState *_state = (AppState *)pvParameters;
            J1939MsgShort *mes;
            J1939Msg *jmsgbig;
            while (1)
            {
                if (_state->j1939module.mesShortQueue != 0)
                {
                    while (xQueueReceive(_state->j1939module.mesShortQueue, &(mes), 0) == pdTRUE)
                    {
                        sendJ1939Mes(mes);
                    }
                }

                if (_state->j1939module.mesLongQueue != 0)
                {

                    while (xQueueReceive(_state->j1939module.mesLongQueue, &(jmsgbig), 0) == pdTRUE)
                    {
                        ESP_LOGW(TAG, "Long queue send pgn %d from %d",jmsgbig->lPGN,jmsgbig->nSrcAddr);
                        sendJ1939Mes(jmsgbig);
                    }
                }

                vTaskDelay(10); /// portTICK_RATE_MS
            }
        }

    };

    esp_err_t Run(AppState *adr)
    {
        state = adr;

        /* URI handler for ws server */
        httpd_uri_t wsendpoint = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = wsendpointhandler,
            .is_websocket = true,
            .handle_ws_control_frames = true};

        httpd_register_uri_handler(adr->sysModule.server, &wsendpoint);

        xTaskCreatePinnedToCore(WsSendTask, "WsSendTask", 10000, adr, 1, NULL, 1);
        // xTaskCreatePinnedToCore(sendPingMesTask, "WsPingTask", 5000, adr, 1, NULL, 0);
        return ESP_OK;
    }

}
