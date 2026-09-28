#include "CommModule.h"

#include "AppData.h"
#include "FieldRegistry.h"
#include "J1939Proto.h"
#include "LogicUtils.h"
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
    ctx_->events.subscribe(APP_EVENTS_BASE, app_event_id_t::COMMUNICATION_SEND,
                           &CommunicationModule::onCommunicationSend, this);
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
    case J1939Proto::kMsgTypeParamRequest:
    {
        // {uid u16 LE} — клиент запрашивает значение поля.
        if (payloadLen != 2)
        {
            ESP_LOGW(TAG, "PARAM_REQUEST payload wrong size (%u B)",
                     (unsigned)payloadLen);
            return;
        }
        const uint16_t uid = static_cast<uint16_t>(payload[0]) |
                             (static_cast<uint16_t>(payload[1]) << 8);
        if (!ctx_->fields.getMetaByUid(uid))
        {
            sendNack(uid, 0, msg->sockfd);   // 0 = unknown
            break;
        }
        sendValueFrame(J1939Proto::kMsgTypeParamAck, uid, msg->sockfd);
        break;
    }
    case J1939Proto::kMsgTypeParamSet:
    {
        // {uid u16 LE, value} — запись поля по протоколу (только config-поля).
        if (payloadLen < 2)
        {
            ESP_LOGW(TAG, "PARAM_SET payload too short (%u B)",
                     (unsigned)payloadLen);
            return;
        }
        const uint16_t uid = static_cast<uint16_t>(payload[0]) |
                             (static_cast<uint16_t>(payload[1]) << 8);
        const FieldMeta* meta = ctx_->fields.getMetaByUid(uid);
        if (!meta)
        {
            sendNack(uid, 0, msg->sockfd);   // 0 = unknown
            break;
        }
        if (payloadLen == 2)
        {
            sendNack(uid, 3, msg->sockfd);   // 3 = len (нет значения)
            break;
        }

        uint8_t oldBuf[kAppMaxFieldSize + 8];
        size_t oldLen = 0;
        ctx_->fields.readField(uid, oldBuf, sizeof(oldBuf), &oldLen);

        const FieldWriteStatus st = ctx_->fields.writeField(
            uid, payload + 2, payloadLen - 2, FieldDomain::PROTOCOL);
        if (st != FieldWriteStatus::OK)
        {
            // 0=unknown 1=readonly 2=range 3=len (FieldWriteStatus минус единица)
            sendNack(uid, static_cast<uint8_t>(st) - 1, msg->sockfd);
            break;
        }

        uint8_t newBuf[kAppMaxFieldSize + 8];
        size_t newLen = 0;
        ctx_->fields.readField(uid, newBuf, sizeof(newBuf), &newLen);
        const bool changed = (oldLen != newLen) ||
                             memcmp(oldBuf, newBuf, oldLen) != 0;

        if (changed)
        {
            postFieldChanged(ctx_, uid);     // автосейв конфига
            sendValueFrame(J1939Proto::kMsgTypeParamPush, uid, -1);  // broadcast
        }
        sendValueFrame(J1939Proto::kMsgTypeParamAck, uid, msg->sockfd);
        break;
    }
    case J1939Proto::kMsgTypeFactoryReset:
    {
        ESP_LOGW(TAG, "FACTORY_RESET requested by WS client %d", msg->sockfd);
        ctx_->events.post(APP_EVENTS_BASE, app_event_id_t::FACTORY_RESET);
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

// Обёртка готового payload в кадр + отправка WS-клиенту(ам).
// sockfd=-1 → broadcast всем, иначе адресная отправка конкретному сокету.
void CommunicationModule::sendFrame(uint16_t msgType, uint8_t flags,
                                    const uint8_t* payload, size_t payloadLen,
                                    int sockfd)
{
    const size_t frameCap = J1939Proto::kHeaderSize + payloadLen +
                            J1939Proto::kCrcSize;
    uint8_t* frame = static_cast<uint8_t*>(malloc(frameCap));
    if (!frame)
        return;

    const size_t frameLen = J1939Proto::wrapFrame(
        msgType, flags, payload, payloadLen, tx_seq_++, frame, frameCap);
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

    msg->sockfd = sockfd;
    msg->length = frameLen;
    memcpy(msg->data, frame, frameLen);
    free(frame);

    ctx_->events.postSized(APP_EVENTS_BASE, app_event_id_t::WS_MESSAGE_SEND,
                           msg, msgSize);
    free(msg);
}

// Отправка значения поля: {uid u16 LE, value} в кадре заданного MsgType.
void CommunicationModule::sendValueFrame(uint16_t msgType, uint16_t uid,
                                         int sockfd)
{
    const FieldMeta* meta = ctx_->fields.getMetaByUid(uid);
    if (!meta)
        return;

    uint8_t payload[2 + kAppMaxFieldSize + 8];
    payload[0] = static_cast<uint8_t>(uid & 0xFF);
    payload[1] = static_cast<uint8_t>(uid >> 8);
    size_t vlen = 0;
    if (!ctx_->fields.readField(uid, payload + 2, sizeof(payload) - 2, &vlen))
        return;
    sendFrame(msgType, 0, payload, 2 + vlen, sockfd);
}

// Ответ об ошибке: {uid u16 LE, err u8}.
void CommunicationModule::sendNack(uint16_t uid, uint8_t err, int sockfd)
{
    uint8_t payload[3];
    payload[0] = static_cast<uint8_t>(uid & 0xFF);
    payload[1] = static_cast<uint8_t>(uid >> 8);
    payload[2] = err;
    sendFrame(J1939Proto::kMsgTypeParamNack, 0, payload, sizeof(payload),
              sockfd);
    ESP_LOGW(TAG, "PARAM_NACK uid=0x%04X err=%u", uid, err);
}

void CommunicationModule::onCommunicationSend(
    const communication_send_event_t* evt)
{
    if (!evt)
        return;
    sendValueFrame(J1939Proto::kMsgTypeParamPush, evt->fieldUid, evt->sockfd);
}

void CommunicationModule::onWsClientConnected(const ws_message_t* msg)
{
    if (!msg)
        return;
    ESP_LOGI(TAG, "WS client %d connected, pushing all fields",
             msg->sockfd);
    // Push-on-connect: адресно отправить текущее значение каждого поля.
    for (size_t i = 0; i < ctx_->fields.fieldCount(); ++i)
    {
        const FieldMeta& m = ctx_->fields.fieldAt(i);
        sendValueFrame(J1939Proto::kMsgTypeParamPush, m.uid, msg->sockfd);
    }
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