#pragma once
#include <cstdint>

#ifndef HOST_TEST
#include "esp_timer.h"
#endif

namespace Timing {
#ifndef HOST_TEST
// Монотонное время в миллисекундах с загрузки (обёртка над esp_timer).
inline uint32_t nowMs()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}
#endif

// Чистая функция тайминга STEP-01: сколько мс ждать до ближайшего события
// (снапшот/телеметрия). 1 = минимальная пауза, чтобы не крутить цикл вхолостую.
// Разность считается через (int32_t) — корректна при переполнении uint32-счётчика
// мс (каждые ~49 суток). Вынесена в header, чтобы покрывалась host-тестами.
inline uint32_t computeWaitMs(uint32_t nextSnapshot, uint32_t nextTelemetry,
                              uint32_t now)
{
    int32_t dSnap = static_cast<int32_t>(nextSnapshot - now);
    int32_t dTele = static_cast<int32_t>(nextTelemetry - now);
    int32_t wait = (dSnap < dTele) ? dSnap : dTele;
    if (wait < 1) return 1;
    return static_cast<uint32_t>(wait);
}

// Период публикации снапшота J1939 (200–500 мс по чекпоинту).
constexpr uint32_t kSnapshotIntervalMs  = 250;
// Не слать записи, которые не обновлялись дольше TTL (активные PGN).
constexpr uint32_t kSnapshotTtlMs       = 2000;
// Таймаут TP-реасемблера (ожидание пакета TP.DT).
constexpr uint32_t kTransportTimeoutMs  = 1000;
// Пауза между проверками в задачах-«пустышках».
constexpr uint32_t kIdleDelayMs         = 10;
}