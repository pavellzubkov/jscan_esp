#pragma once
// Заглушка freertos/queue.h для host-тестов: минимальная header-only
// кольцевая очередь — нужна для J1939MsgChannel (контракт push/pop/drain).
// Блокировок в тестах нет: ненулевой ticks не ждёт — при пустой очереди
// (receive) или полной (send) немедленный pdFALSE.
#include "freertos/FreeRTOS.h"
#include <cstdlib>
#include <cstring>

struct HostStubQueue {
    uint8_t* storage;
    UBaseType_t itemSize;
    UBaseType_t capacity;
    UBaseType_t count;
    UBaseType_t head;   // индекс чтения
    UBaseType_t tail;   // индекс записи
};

inline QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t itemSize)
{
    if (!len || !itemSize)
        return nullptr;
    auto* q = static_cast<HostStubQueue*>(std::malloc(sizeof(HostStubQueue)));
    if (!q)
        return nullptr;
    q->storage =
        static_cast<uint8_t*>(std::malloc(static_cast<size_t>(len) * itemSize));
    if (!q->storage)
    {
        std::free(q);
        return nullptr;
    }
    q->itemSize = itemSize;
    q->capacity = len;
    q->count = 0;
    q->head = 0;
    q->tail = 0;
    return q;
}

// Полна → дроп, без блокировки (ticks игнорируется).
inline BaseType_t xQueueSend(QueueHandle_t handle, const void* item, TickType_t)
{
    auto* q = static_cast<HostStubQueue*>(handle);
    if (!q || !item)
        return pdFALSE;
    if (q->count >= q->capacity)
        return pdFALSE;
    std::memcpy(q->storage + q->tail * q->itemSize, item, q->itemSize);
    q->tail = (q->tail + 1) % q->capacity;
    ++q->count;
    return pdTRUE;
}

// Пуста → немедленный pdFALSE, сколько бы ticks ни передано (блокировок нет).
inline BaseType_t xQueueReceive(QueueHandle_t handle, void* out, TickType_t)
{
    auto* q = static_cast<HostStubQueue*>(handle);
    if (!q || !out)
        return pdFALSE;
    if (q->count == 0)
        return pdFALSE;
    std::memcpy(out, q->storage + q->head * q->itemSize, q->itemSize);
    q->head = (q->head + 1) % q->capacity;
    --q->count;
    return pdTRUE;
}

inline void vQueueDelete(QueueHandle_t handle)
{
    auto* q = static_cast<HostStubQueue*>(handle);
    if (!q)
        return;
    std::free(q->storage);
    std::free(q);
}
