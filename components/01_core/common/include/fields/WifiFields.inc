// @label Имя AP
// @description Имя точки доступа (SSID)
// @group Access Point
DATA_FIELD(FixedString, apSsid,           "J1939_AP",  1, 31, CFG_STRING, true, false)

// @label Пароль AP
// @description Пароль точки доступа (пустой = открытая сеть)
// @group Access Point
// @sensitive true
DATA_FIELD(FixedString, apPassword,       "12345678",  0, 31, CFG_STRING, true, false)

// @label Канал AP
// @description Радиоканал точки доступа
// @group Access Point
DATA_FIELD(uint8_t,     apChannel,        6, 1, 11, CFG_UINT, true, false)

// @label Макс. клиентов
// @description Максимальное число подключённых клиентов
// @group Access Point
DATA_FIELD(uint8_t,     maxStaConn,       2, 1, 8, CFG_UINT, true, false)

// @label IP точки доступа
// @description Статический IP адрес точки доступа
// @group Access Point
DATA_FIELD(FixedString, apIp,             "10.10.10.10", 7, 15, CFG_IP, true, false)

// @label Клиенты WiFi
// @description Текущее число подключённых клиентов
// @group Access Point
DATA_FIELD(uint8_t,     wifiClients,      0, 0, 10, CFG_UINT, false, true)

// @label Режим AP
// @description Активен ли режим точки доступа
// @group Access Point
DATA_FIELD(bool,        wifiApMode,       true, 0, 1, CFG_BOOL, false, true)