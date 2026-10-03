#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <atomic>
#include <cstdint>
#include <cstdlib>

// Сообщение «готовый PGN → сканер»: одиночный кадр либо собранный TP-пакет.
// Данные: len <= 8 — inline smallData; длинные (TP до 1785 байт) — heap-буфер
// bigData (одиночный кадр в очереди не должен тащить 1785-байтовый хвост).
struct j1939_msg_t {
    uint32_t tsMs;         // Timing::nowMs() приёма (монотонный, для periodMs)
    uint32_t pgn;
    uint16_t len;
    uint8_t  sa;
    uint8_t  smallData[8];
    uint8_t* bigData;      // nullptr, если len <= 8

    const uint8_t* data() const { return bigData ? bigData : smallData; }
};

// SPSC-канал J1939System (продьюсер, задача j1939) → J1939Scanner
// (потребитель, задача scanner). Живёт в AppContext — осознанное исключение
// из правила «модули общаются только через event bus»: горячий поток PGN
// (до кадра каждые 10 мс) не должен копировать байты в event loop
// (queue_size=256, postSized до 8 КБ на событие). Обратный канал
// (сканер → J1939, команды) остаётся событийным — он редкий.
//
// Политика: продьюсер не блокируется (timeout 0); очередь полна или
// потребителя нет — дроп НОВЕЙШЕГО + счётчик drops. Владение bigData:
// push() забирает буфер всегда (при неудаче освобождает сам), pop() отдаёт
// владение потребителю (обязан free после копии в аккумулятор).
class J1939MsgChannel {
public:
    static constexpr uint32_t kDepth = 32;   // ~1 КБ RAM (32 * sizeof(j1939_msg_t))

    J1939MsgChannel()
        : queue_(xQueueCreate(kDepth, sizeof(j1939_msg_t)))
    {
    }
    ~J1939MsgChannel()
    {
        drain();
        if (queue_)
        {
            vQueueDelete(queue_);
            queue_ = nullptr;
        }
    }
    J1939MsgChannel(const J1939MsgChannel&) = delete;
    J1939MsgChannel& operator=(const J1939MsgChannel&) = delete;

    // Потребитель встал (begin сканера) / ушёл (dtor). Без потребителя
    // push не кладёт — иначе bigData копились бы в непроточной очереди.
    void setConsumer(bool on) { consumer_.store(on, std::memory_order_release); }
    bool consumerActive() const { return consumer_.load(std::memory_order_acquire); }

    // Положить сообщение. После вызова продьютер не трогает m.bigData:
    // канал либо передал буфер в очередь, либо освободил сам.
    bool push(const j1939_msg_t& m)
    {
        j1939_msg_t copy = m;
        if (!queue_ || !consumerActive())
        {
            freeBig(copy);
            drops_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (xQueueSend(queue_, &copy, 0) != pdTRUE)
        {
            freeBig(copy);
            drops_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    // Забрать сообщение; waitMs — таймаут блокировки потребителя.
    // Владение out.bigData переходит потребителю.
    bool pop(j1939_msg_t* out, uint32_t waitMs)
    {
        if (!queue_ || !out)
            return false;
        return xQueueReceive(queue_, out, pdMS_TO_TICKS(waitMs)) == pdTRUE;
    }

    uint32_t drops() const { return drops_.load(std::memory_order_relaxed); }

private:
    static void freeBig(j1939_msg_t& m)
    {
        std::free(m.bigData);
        m.bigData = nullptr;
    }

    // Освободить хвост очереди (dtor): недочитанные bigData не должны течь.
    void drain()
    {
        if (!queue_)
            return;
        j1939_msg_t m;
        while (xQueueReceive(queue_, &m, 0) == pdTRUE)
            freeBig(m);
    }

    QueueHandle_t queue_;
    std::atomic<bool> consumer_{false};
    std::atomic<uint32_t> drops_{0};
};