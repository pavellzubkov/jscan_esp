// @label Адрес узла
// @description Адрес узла в J1939-сети (src при отправке RQST)
// @group TWAI
DATA_FIELD(uint8_t,     canNodeAddr,      Hw::kDefaultNodeAddr, 0, 253, CFG_UINT, true, false)

// @label Битрейт CAN
// @description Битрейт шины (125000/250000/500000/1000000); применяется при перезагрузке
// @group TWAI
// @unit bps
DATA_FIELD(uint32_t,    canBitrate,       Hw::kCanBitrate, 125000, 1000000, CFG_UINT, true, false)

// @label Таймаут TX
// @description Таймаут передачи кадра CAN
// @group TWAI
// @unit мс
DATA_FIELD(uint16_t,    canTxTimeoutMs,   Hw::kDefaultTxTimeoutMs, 10, 500, CFG_UINT, true, false)

// @label Автовосстановление
// @description Автоматический recover драйвера при BUS_OFF
// @group TWAI
DATA_FIELD(bool,        canAutoRecover,   true, 0, 1, CFG_BOOL, true, false)

// @label Состояние TWAI
// @description 0=STOPPED 1=RUNNING 2=BUS_OFF 3=RECOVERING
// @group TWAI
DATA_FIELD(uint8_t,     twaiState,        0, 0, 3, CFG_UINT, false, true)

// @label Ошибки TX
// @description Счётчик ошибок передачи
// @group TWAI
DATA_FIELD(uint8_t,     twaiTxErr,        0, 0, 255, CFG_UINT, false, true)

// @label Ошибки RX
// @description Счётчик ошибок приёма
// @group TWAI
DATA_FIELD(uint8_t,     twaiRxErr,        0, 0, 255, CFG_UINT, false, true)

// @label Восстановлений
// @description Число автоматических recover после BUS_OFF
// @group TWAI
DATA_FIELD(uint32_t,    twaiRecoverCount, 0, 0, 0xFFFFFFFF, CFG_UINT, false, true)

// @label Потери RX
// @description Кадры, потерянные приёмом (нет свободного слота пула ISR)
// @group TWAI
DATA_FIELD(uint32_t,    twaiRxDrops,      0, 0, 0xFFFFFFFF, CFG_UINT, false, true)

// @label Потери TX
// @description Кадры, не поставленные в очередь передачи (пул TX исчерпан)
// @group TWAI
DATA_FIELD(uint32_t,    twaiTxDrops,      0, 0, 0xFFFFFFFF, CFG_UINT, false, true)

// @label TWAI запущен
// @description Флаг запущенного драйвера TWAI
// @group TWAI
DATA_FIELD(bool,        twaiStarted,      false, 0, 1, CFG_BOOL, false, true)