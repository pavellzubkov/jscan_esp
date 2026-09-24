#pragma once
#include <cstdint>

namespace Timing {
// Период публикации снапшота J1939 (200–500 мс по чекпоинту).
constexpr uint32_t kSnapshotIntervalMs  = 250;
// Не слать записи, которые не обновлялись дольше TTL (активные PGN).
constexpr uint32_t kSnapshotTtlMs       = 2000;
// Таймаут TP-реасемблера (ожидание пакета TP.DT).
constexpr uint32_t kTransportTimeoutMs  = 1000;
// Пауза между проверками в задачах-«пустышках».
constexpr uint32_t kIdleDelayMs         = 10;
}