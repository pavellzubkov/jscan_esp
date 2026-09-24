#include "CommModule.h"

#include "J1939Proto.h"
#include "esp_log.h"
#include <cstdlib>
#include <cstring>

namespace {
const char* TAG = "Comm";
}

CommunicationModule::CommunicationModule(AppContext* ctx)
    : ctx_(ctx)
{
}

esp_err_t CommunicationModule::begin()
{
    ctx_->events.subscribe(APP_EVENTS_BASE, app_event_id_t::WS_MESSAGE_RECEIVED,
                           &CommunicationModule::onIncomingPacket, this);
    ctx_->events.subscribe(APP_EVENTS_BASE, app_event_id_t::J1939_SNAPSHOT_SEND,
                           &CommunicationModule::onSnapshot, this);
    ctx_->events.subscribe(APP_EVENTS_BASE, app_event_id_t::WS_CLIENT_CONNECTED,
                           &CommunicationModule::onWsClientConnected, this);
    ctx_->events.subscribe(APP_EVENTS_BASE, app_event_id_t::WS_CLIENT_DISCONNECTED,
                           &CommunicationModule::onWsClientDisconnected, this);
    ctx_->events.subscribe(APP_EVENTS_BASE, app_event_id_t::WIFI_STATUS,
                           &CommunicationModule::onWifiStatus, this);
    ESP_LOGI(TAG, "CommunicationModule ready");
    return ESP_OK;
}

void CommunicationModule::onIncomingPacket(const ws_message_t* msg)
{
    if (!msg || msg->length == 0)
        return;

    uint16_t msgType = 0;
    uint8_t flags = 0;
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;

    if (!J1939Proto::unwrapFrame(reinterpret_cast<const uint8_t*>(msg->data),
                                 msg->length, &msgType, &flags, &payload,
                                 &payloadLen))
    {
        ESP_LOGW(TAG, "bad frame from sockfd=%d len=%u", msg->sockfd,
                 (unsigned)msg->length);
        return;
    }

    switch (msgType)
    {
    case J1939Proto::kMsgTypeRequest:
    {
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
        ESP_LOGI(TAG, "RQST pgn=%lu dst=%u", (unsigned long)req.pgn,
                 req.dstAddr);
        ctx_->events.post(APP_EVENTS_BASE, app_event_id_t::J1939_REQUEST, req);
        break;
    }
    default:
        ESP_LOGW(TAG, "unknown MsgType 0x%04X", msgType);
        break;
    }
}

void CommunicationModule::onSnapshot(const j1939_snapshot_t* snap)
{
    if (!snap || snap->length == 0)
        return;

    const size_t frameCap = J1939Proto::kHeaderSize + snap->length +
                            J1939Proto::kCrcSize;
    uint8_t* frame = static_cast<uint8_t*>(malloc(frameCap));
    if (!frame)
        return;

    const size_t frameLen = J1939Proto::wrapFrame(
        J1939Proto::kMsgTypeSnapshot, J1939Proto::kFlagSnapshot, snap->data,
        snap->length, tx_seq_++, frame, frameCap);
    if (frameLen == 0)
    {
        free(frame);
        return;
    }

    const size_t msgSize = sizeof(ws_message_t) + frameLen;
    ws_message_t* msg = static_cast<ws_message_t*>(malloc(msgSize));
    if (!msg)
    {
        free(frame);
        return;
    }

    msg->sockfd = -1;   // broadcast всем WS-клиентам
    msg->length = frameLen;
    memcpy(msg->data, frame, frameLen);
    free(frame);

    ctx_->events.postSized(APP_EVENTS_BASE, app_event_id_t::WS_MESSAGE_SEND,
                           msg, msgSize);
    free(msg);

    ESP_LOGD(TAG, "snapshot %u B, n=%u", (unsigned)snap->length,
             (unsigned)snap->data[0]);
}

void CommunicationModule::onWsClientConnected(const ws_message_t* msg)
{
    if (msg)
        ESP_LOGI(TAG, "WS client %d connected", msg->sockfd);
}

void CommunicationModule::onWsClientDisconnected(const ws_message_t* msg)
{
    if (msg)
        ESP_LOGI(TAG, "WS client %d disconnected", msg->sockfd);
}

void CommunicationModule::onWifiStatus(const wifi_status_event_t* s)
{
    if (s)
        ESP_LOGI(TAG, "WIFI ap=%d clients=%u", s->is_ap_mode ? 1 : 0,
                 s->num_clients);
}