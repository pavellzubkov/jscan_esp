#include "CommModule.h"

#include "AppData.h"
#include "ByteOrder.h"
#include "FieldRegistry.h"
#include "J1939Proto.h"
#include "LogicUtils.h"
#include "esp_log.h"
#include <cstring>

namespace {
const char* TAG = "Comm";

// Коды ошибок PARAM_NACK (PROTOCOL-J1939 §5): 0=unknown 1=readonly
// 2=range 3=len. Соответствие FieldWriteStatus — только здесь.
uint8_t nackCodeFromStatus(FieldWriteStatus st)
{
    switch (st)
    {
    case FieldWriteStatus::UNKNOWN_UID:     return 0;
    case FieldWriteStatus::READONLY_DENIED: return 1;
    case FieldWriteStatus::OUT_OF_RANGE:    return 2;
    case FieldWriteStatus::BAD_LENGTH:      return 3;
    default:                                return 0;
    }
}
}

CommunicationModule::CommunicationModule(AppContext* ctx)
    : ctx_(ctx), tx_(ctx), j1939_(ctx, &tx_)
{
}

CommunicationModule::~CommunicationModule()
{
    // Подписки снимаются всегда: makeModule удаляет объект при ошибке begin()
    // — без отписки висячий обработчик получил бы событие на удалённый объект.
    if (ctx_)
        ctx_->events.unsubscribe(this);
}

esp_err_t CommunicationModule::begin()
{
    const bool subsOk =
        subscribeEvent<app_event_id_t::WS_MESSAGE_RECEIVED>(
            ctx_->events, &CommunicationModule::onIncomingPacket, this) &&
        subscribeEvent<app_event_id_t::WS_CLIENT_CONNECTED>(
            ctx_->events, &CommunicationModule::onWsClientConnected, this) &&
        subscribeEvent<app_event_id_t::WS_CLIENT_DISCONNECTED>(
            ctx_->events, &CommunicationModule::onWsClientDisconnected, this) &&
        subscribeEvent<app_event_id_t::WIFI_STATUS>(
            ctx_->events, &CommunicationModule::onWifiStatus, this) &&
        subscribeEvent<app_event_id_t::COMMUNICATION_SEND>(
            ctx_->events, &CommunicationModule::onCommunicationSend, this);
    if (!subsOk)
    {
        ESP_LOGE(TAG, "event subscribe failed, abort start");
        return ESP_FAIL;
    }
    // J1939-ветка (снапшоты + аудитория WS): при ошибке откат делает dtor
    // (снимает подписки обоих объектов).
    if (j1939_.begin() != ESP_OK)
        return ESP_FAIL;
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

    // Диспатч по таблице kCmds: валидация длины payload (minLen/exact) —
    // в одном месте, без ручных проверок в каждом обработчике.
    for (const CmdDesc& c : kCmds)
    {
        if (c.type != msgType)
            continue;
        if (payloadLen < c.minLen || (c.exact && payloadLen != c.minLen))
        {
            ESP_LOGW(TAG, "MsgType 0x%04X payload wrong size (%u B, expected %s%u)",
                     msgType, (unsigned)payloadLen,
                     c.exact ? "exactly " : "at least ", (unsigned)c.minLen);
            return;
        }
        (this->*c.fn)(payload, payloadLen, msg->sockfd);
        return;
    }
    ESP_LOGW(TAG, "unknown MsgType 0x%04X", msgType);
}

// J1939-ветка протокола — в подмодуле (парсинг + post J1939_REQUEST).
void CommunicationModule::onCmdJ1939Request(const uint8_t* payload, size_t len,
                                             int /*sockfd*/)
{
    j1939_.onClientRequest(payload, len);
}

// {uid u16 LE} — клиент запрашивает значение поля (длина ровно 2, см. kCmds).
void CommunicationModule::onCmdParamRequest(const uint8_t* payload, size_t /*len*/,
                                             int sockfd)
{
    const uint16_t uid = readU16LE(payload);
    if (!ctx_->fields.getMetaByUid(uid))
    {
        sendNack(uid, 0, sockfd);   // 0 = unknown
        return;
    }
    sendValueFrame(J1939Proto::kMsgTypeParamAck, uid, sockfd);
}

// {uid u16 LE, value} — запись поля по протоколу (только config-поля).
void CommunicationModule::onCmdParamSet(const uint8_t* payload, size_t len,
                                        int sockfd)
{
    const uint16_t uid = readU16LE(payload);
    const FieldMeta* meta = ctx_->fields.getMetaByUid(uid);
    if (!meta)
    {
        sendNack(uid, 0, sockfd);   // 0 = unknown
        return;
    }
    if (len == 2)
    {
        sendNack(uid, 3, sockfd);   // 3 = len (нет значения)
        return;
    }

    bool changed = false;
    const FieldWriteStatus st = ctx_->fields.writeFieldDetectChange(
        uid, payload + 2, len - 2, FieldDomain::PROTOCOL, &changed);
    if (st != FieldWriteStatus::OK)
    {
        sendNack(uid, nackCodeFromStatus(st), sockfd);
        return;
    }

    if (changed)
    {
        postFieldChanged(ctx_, uid);     // автосейв конфига
        sendValueFrame(J1939Proto::kMsgTypeParamPush, uid, -1);  // broadcast
    }
    sendValueFrame(J1939Proto::kMsgTypeParamAck, uid, sockfd);
}

void CommunicationModule::onCmdFactoryReset(const uint8_t* /*payload*/,
                                            size_t /*len*/, int sockfd)
{
    ESP_LOGW(TAG, "FACTORY_RESET requested by WS client %d", sockfd);
    postEvent<app_event_id_t::FACTORY_RESET>(ctx_->events);
}

// Отправка значения поля: {uid u16 LE, value} в кадре заданного MsgType.
void CommunicationModule::sendValueFrame(uint16_t msgType, uint16_t uid,
                                         int sockfd)
{
    const FieldMeta* meta = ctx_->fields.getMetaByUid(uid);
    if (!meta)
        return;

    uint8_t payload[2 + kAppMaxFieldSize + 8];
    writeU16LE(payload, uid);
    size_t vlen = 0;
    if (!ctx_->fields.readField(uid, payload + 2, sizeof(payload) - 2, &vlen))
        return;
    tx_.send(msgType, 0, payload, 2 + vlen, sockfd);
}

// Ответ об ошибке: {uid u16 LE, err u8}.
void CommunicationModule::sendNack(uint16_t uid, uint8_t err, int sockfd)
{
    uint8_t payload[3];
    writeU16LE(payload, uid);
    payload[2] = err;
    tx_.send(J1939Proto::kMsgTypeParamNack, 0, payload, sizeof(payload),
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
        const FieldMeta* m = ctx_->fields.fieldAt(i);
        if (!m) continue;
        sendValueFrame(J1939Proto::kMsgTypeParamPush, m->uid, msg->sockfd);
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