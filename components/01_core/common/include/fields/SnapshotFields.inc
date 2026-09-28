// @label Интервал снапшота
// @description Период публикации батч-фрейма J1939
// @group Snapshot
// @unit мс
DATA_FIELD(uint32_t,    snapshotIntervalMs, 250, 100, 1000, CFG_UINT, true, false)

// @label TTL снапшота
// @description Время «активности» PGN в аккумуляторе
// @group Snapshot
// @unit мс
DATA_FIELD(uint32_t,    snapshotTtlMs,      2000, 500, 10000, CFG_UINT, true, false)

// @label Макс. PGN
// @description Потолок карты аккумулятора (буфер фиксирован 128)
// @group Snapshot
DATA_FIELD(uint16_t,    maxTrackedPgns,     128, 16, 128, CFG_UINT, true, false)

// @label Активные PGN
// @description Текущее число отслеживаемых PGN
// @group Snapshot
DATA_FIELD(uint16_t,    activePgns,         0, 0, 128, CFG_UINT, false, true)