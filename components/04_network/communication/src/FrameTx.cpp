#include "FrameTx.hpp"

#include "J1939Proto.hpp"
#include <cstdlib>
#include <cstring>
#include <memory>

// malloc + unique_ptr с deleter free: выравнивание max_align_t под
// ws_message_t (make_unique<uint8_t[]> дал бы выравнивание 1) + RAII.
using BufPtr = std::unique_ptr<uint8_t, decltype(&std::free)>;

// Обёртка готового payload в кадр + отправка WS-клиенту(ам).
// Путь: один буфер (ws_message_t + место под кадр) → wrapFrame прямо в
// msg->data (второго malloc и копирования frame→msg больше нет) →
// postSized (событие копируется в очередь event loop'ом) → буфер
// освобождает unique_ptr сам на любом пути выхода (раньше здесь было два
// malloc и три ручных free — один добавленный return = утечка).
void FrameTx::send(uint16_t msgType, uint8_t flags, const uint8_t* payload,
                   size_t payloadLen, int sockfd)
{
    const size_t frameCap = J1939Proto::kHeaderSize + payloadLen +
                            J1939Proto::kCrcSize;
    BufPtr buf(static_cast<uint8_t*>(std::malloc(sizeof(ws_message_t) +
                                                 frameCap)),
               &std::free);
    if (!buf)
        return;
    auto* msg = reinterpret_cast<ws_message_t*>(buf.get());

    const size_t frameLen = J1939Proto::wrapFrame(
        msgType, flags, payload, payloadLen, seq_++,
        reinterpret_cast<uint8_t*>(msg->data), frameCap);
    if (frameLen == 0)
        return;   // буфер освободит unique_ptr

    msg->sockfd = sockfd;
    msg->length = frameLen;

    postSizedEvent<app_event_id_t::WS_MESSAGE_SEND>(
        ctx_->events, msg, sizeof(ws_message_t) + frameLen);
    // postSizedEvent копировал данные в очередь — буфер здесь уже не нужен;
    // освободится автоматически (и на путях ошибок выше — тоже).
}
