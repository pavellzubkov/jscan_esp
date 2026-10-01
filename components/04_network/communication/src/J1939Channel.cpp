#include "J1939Channel.hpp"

#include "FrameTx.hpp"
#include "J1939Proto.h"
#include "esp_log.h"

namespace {
const char* TAG = "j1939_chan";
}

J1939Channel::J1939Channel(AppContext* ctx, FrameTx* tx)
    : ctx_(ctx), tx_(tx)
{
}

J1939Channel::~J1939Channel()
{
    // Подписки снимаются всегда: makeModule удаляет CommModule при ошибке
    // begin() — без отписки висячий обработчик получил бы событие на
    // удалённый объект (подписки числятся за this, не за владельцем).
    if (ctx_)
        ctx_->events.unsubscribe(this);
}

esp_err_t J1939Channel::begin()
{
    const bool subsOk =
        subscribeEvent<app_event_id_t::J1939_SNAPSHOT_SEND>(
            ctx_->events, &J1939Channel::onSnapshot, this) &&
        subscribeEvent<app_event_id_t::WS_CLIENT_CONNECTED>(
            ctx_->events, &J1939Channel::onClientConnected, this) &&
        subscribeEvent<app_event_id_t::WS_CLIENT_DISCONNECTED>(
            ctx_->events, &J1939Channel::onClientDisconnected, this);
    if (!subsOk)
    {
        ESP_LOGE(TAG, "event subscribe failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}

void J1939Channel::onSnapshot(const j1939_snapshot_t* snap)
{
    if (!snap || snap->length == 0)
        return;
    if (clients_ <= 0)
        return;   // аудитории нет — кадр не строим и не отправляем

    ESP_LOGD(TAG, "snapshot %u B, n=%u", (unsigned)snap->length,
             (unsigned)snap->data[0]);
    tx_->send(J1939Proto::kMsgTypeSnapshot, J1939Proto::kFlagSnapshot,
              snap->data, snap->length, -1);
}

void J1939Channel::onClientConnected(const ws_message_t* msg)
{
    if (!msg)
        return;
    ++clients_;
    ESP_LOGI(TAG, "WS client %d connected, listeners=%d", msg->sockfd,
             clients_);
}

void J1939Channel::onClientDisconnected(const ws_message_t* msg)
{
    if (!msg)
        return;
    if (clients_ > 0)
        --clients_;   // защита от рассинхронизации событий (<0 недопустим)
    ESP_LOGI(TAG, "WS client %d disconnected, listeners=%d", msg->sockfd,
             clients_);
}

void J1939Channel::onClientRequest(const uint8_t* payload, size_t payloadLen)
{
    if (!payload)
        return;
    if (payloadLen < 5)
    {
        ESP_LOGW(TAG, "J1939_REQUEST payload too short (%u B)",
                 (unsigned)payloadLen);
        return;
    }
    j1939_request_t req;
    req.dstAddr = payload[0];
    req.pgn = static_cast<uint32_t>(payload[1]) |
              (static_cast<uint32_t>(payload[2]) << 8) |
              (static_cast<uint32_t>(payload[3]) << 16);
    ESP_LOGI(TAG, "RQST pgn=%lu dst=%u", (unsigned long)req.pgn, req.dstAddr);
    postEvent<app_event_id_t::J1939_REQUEST>(ctx_->events, req);
}
