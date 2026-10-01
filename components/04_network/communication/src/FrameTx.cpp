#include "FrameTx.hpp"

#include "J1939Proto.h"
#include <cstdlib>
#include <cstring>

// Обёртка готового payload в кадр + отправка WS-клиенту(ам).
// Путь один для всех каналов: malloc frame → wrapFrame → malloc ws_message →
// postSized → free. Копирование два (frame → msg) — как раньше, отдавать
// некопированный буфер в postSized нельзя: событие должно пережить free.
void FrameTx::send(uint16_t msgType, uint8_t flags, const uint8_t* payload,
                   size_t payloadLen, int sockfd)
{
    const size_t frameCap = J1939Proto::kHeaderSize + payloadLen +
                            J1939Proto::kCrcSize;
    uint8_t* frame = static_cast<uint8_t*>(malloc(frameCap));
    if (!frame)
        return;

    const size_t frameLen = J1939Proto::wrapFrame(
        msgType, flags, payload, payloadLen, seq_++, frame, frameCap);
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
