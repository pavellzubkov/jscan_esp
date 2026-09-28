// @label Версия FW
// @description Версия прошивки
// @group System
DATA_FIELD(FixedString, fwVersion,        Hw::kFwVersion, 1, 15, CFG_STRING, false, true)

// @label Аптайм
// @description Время работы с момента загрузки
// @group System
// @unit мс
DATA_FIELD(uint32_t,    uptimeMs,         0, 0, 0xFFFFFFFF, CFG_UINT, false, true)

// @label Свободный heap
// @description Свободная память кучи
// @group System
// @unit байт
DATA_FIELD(uint32_t,    heapFree,         0, 0, 0xFFFFFFFF, CFG_UINT, false, true)