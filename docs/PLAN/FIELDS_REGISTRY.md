# План: реестр полей (.inc) + WS-канал параметров + factory reset

Проект: jscan_esp (ESP32-S3, ESP-IDF 6.1, C++17).
Референс по механизму .inc — TEMP_PID (`E:\Projects\Embedded\ESP32\Temp_pid\TEMP_PID`).

> ## Регламент выполнения (ВАЖНО)
> - Шаги выполняются **строго по одному**, каждый в отдельном контексте.
> - После завершения шага обязательно прогнать сборку (`idf.py reconfigure && idf.py build`).
> - Только после **успешной сборки** — пометить шаг `[x]` (completed) в чеклисте ниже
>   и **ОСТАНОВИТЬСЯ**. Следующий шаг начинается по явной команде пользователя.
> - Не начинать следующий шаг, пока предыдущий не помечен `[x]`.

## Статус шагов (чеклист)

- [x] STEP-01 — Ядро реестра (01_core/common): AppTypes / DataFields.inc / AppData / FieldRegistry + AppContext
- [x] STEP-02 — ConfigStore на реестре + factory reset (05_storage/config_store)
- [ ] STEP-03 — Протокол: новые MsgType + CommModule (01_core/common, 04_network/communication)
- [ ] STEP-04 — TWAI runtime + конфиг (02_hardware/twai, 03_systems/j1939_system)
- [ ] STEP-05 — WifiApModule: live-apply + runtime-поля (04_network/wifi)
- [ ] STEP-06 — SystemStatusModule (опционально, новый 03_systems/system_status)
- [ ] STEP-07 — Документация (docs/PROTOCOL-J1939.md, AGENTS.md)
- [ ] STEP-08 — Финальная сборка и проверка

---

## Цель
Внедрить реестр именованных полей (X-macro .inc, как в TEMP_PID) для
конфигурационных и рантайм-параметров модулей, добавить WS-канал их
чтения/записи поверх текущего MsgType-протокола, и подготовить механизм
factory reset (сброс конфига к дефолтам), который позже можно повесить на кнопку.

## Зафиксированные решения (согласовано с пользователем)
1. WS-канал параметров — новые MsgType, UID поля в payload (2B LE).
   J1939-стриминг (0x0001/0x0002) не трогаем.
2. AP-конфиг — live-apply: WifiApModule перезапускает softAP по CONFIG_CHANGED.
3. TWAI — canNodeAddr/canTxTimeoutMs/canAutoRecover применяются сразу;
   canBitrate — при перезагрузке (драйвер вживую не дёргаем).
4. Старый config.json — сброс к дефолтам, без миграции (неизвестные ключи
   игнорируются, дефолты применяются, при сейве файл перезапишется в новом формате).
5. Фронтенд — НЕ трогаем в этой итерации (отдельный репозиторий, отдельный этап).

## Текущее состояние (до начала работ)
- Конфиг — статическая структура `AppConfig` (`components/01_core/common/include/AppConfig.h`),
  читается 1 раз при загрузке из `/config/config.json` вручную (`ConfigStore::loadFromFs`).
- WS-протокол MsgType-based: 0x0001 SNAPSHOT (батч), 0x0002 REQUEST (`CommModule.cpp:52-75`).
- Канал параметров отсутствует. `WIFI_STATUS`, `CONFIG_CHANGED` — задел без подписчиков.
- `Hw::kJ1939MyAddr=25` — мёртвая константа (нигде не используется; RQST шлёт src 0xFF).
- TWAI стартует 1 раз в `J1939System::begin()`; runtime-состояние шины не публикуется.
- EventManager pool = 64 слота, занято ~30 (запас есть).
- Зависимости CMake (REQUIRES): 01_core/common = `esp_event freertos log`;
  config_store = `common littlefs_service espressif__cjson freertos log nvs_flash`;
  wifi = `common esp_wifi esp_netif esp_event esp_driver_gpio freertos lwip log`;
  j1939_system = `common twai freertos driver esp_driver_twai esp_driver_gpio esp_common esp_event log`;
  communication = `common freertos log`.

## Схема реестра (4 домена, `DataFields.inc`)

UID = `fnv1a32(name)&0xFFFF`; коллизии и 0xFFFF ловит static_assert.
Строки — `FixedString[32]`. Сериализация по PROTOCOL §2 (строки `[len][data]`,
числа raw LE, float IEEE-754 LE).

### WIFI (владелец runtime — WifiApModule)
| поле | тип | def | min/max | val | cfg | ro |
|---|---|---|---|---|---|---|
| apSsid | FixedString | "J1939_AP" | 1..31 | STRING | да | нет |
| apPassword | FixedString | "12345678" | 0..31 | STRING(@sensitive) | да | нет |
| apChannel | uint8 | 6 | 1..11 | UINT | да | нет |
| maxStaConn | uint8 | 2 | 1..8 | UINT | да | нет |
| apIp | FixedString | "10.10.10.10" | 7..15 | IP | да | нет |
| wifiClients | uint8 | 0 | 0..10 | UINT | нет | да |
| wifiApMode | bool | true | 0..1 | BOOL | нет | да |

### TWAI (владелец runtime — J1939System)
| поле | тип | def | min/max | val | cfg | ro |
|---|---|---|---|---|---|---|
| canNodeAddr | uint8 | 25 | 0..253 | UINT | да | нет |
| canBitrate | uint32 | 250000 | 125000..1000000 | UINT | да | нет | применяется при перезагрузке |
| canTxTimeoutMs | uint16 | 100 | 10..500 | UINT | да | нет |
| canAutoRecover | bool | true | 0..1 | BOOL | да | нет |
| twaiState | uint8 | 0 | 0..3 | UINT | нет | да | 0=STOPPED 1=RUNNING 2=BUS_OFF 3=RECOVERING |
| twaiTxErr | uint8 | 0 | 0..255 | UINT | нет | да |
| twaiRxErr | uint8 | 0 | 0..255 | UINT | нет | да |
| twaiRecoverCount | uint32 | 0 | 0..0xFFFFFFFF | UINT | нет | да |
| twaiStarted | bool | false | 0..1 | BOOL | нет | да |

### SNAPSHOT (владелец runtime — J1939System)
| поле | тип | def | min/max | val | cfg | ro |
|---|---|---|---|---|---|---|
| snapshotIntervalMs | uint32 | 250 | 100..1000 | UINT | да | нет |
| snapshotTtlMs | uint32 | 2000 | 500..10000 | UINT | да | нет |
| maxTrackedPgns | uint16 | 128 | 16..128 | UINT | да | нет | буфер аккумулятора фиксирован 128 |
| activePgns | uint16 | 0 | 0..128 | UINT | нет | да |

### SYSTEM (владелец runtime — SystemStatusModule, опционально)
| поле | тип | def | min/max | val | cfg | ro |
|---|---|---|---|---|---|---|
| fwVersion | FixedString | "1.0.0" | 1..15 | STRING | нет | да |
| uptimeMs | uint32 | 0 | 0..0xFFFFFFFF | UINT | нет | да |
| heapFree | uint32 | 0 | 0..0xFFFFFFFF | UINT | нет | да |

## Протокол: новые MsgType (кадр НЕ меняется)
- `0x0003 PARAM_REQUEST` client→ESP: `{uid u16 LE}`
- `0x0004 PARAM_SET` client→ESP: `{uid u16 LE, value}`
- `0x0005 PARAM_ACK` ESP→client: `{uid u16 LE [, value — для REQUEST]}`
- `0x0006 PARAM_NACK` ESP→client: `{uid u16 LE, err u8}` (0=unknown 1=readonly 2=range 3=len)
- `0x0007 PARAM_PUSH` ESP→client: `{uid u16 LE, value}`
- `0x0008 FACTORY_RESET` client→ESP: пустой payload

## Потоки данных
- Запись: клиент SET → CommModule → `fields.writeField(uid, ..., FieldDomain::PROTOCOL)`
  → если изменилось: `CONFIG_CHANGED{uid}` + broadcast PUSH + ACK клиенту.
- Персист: ConfigStore подписан на CONFIG_CHANGED → `dirty_=true` → автосейв JSON.
- Применение: владельцы доменов подписаны на CONFIG_CHANGED и применяют своё.
- Телеметрия: владельцы пишут runtime-поля через `writeFieldScalar` и шлют
  `COMMUNICATION_SEND{uid, sockfd}` → CommModule → PUSH (broadcast/-1 или адресно).
- Push-on-connect: WS_CLIENT_CONNECTED → CommModule шлёт PUSH всех полей адресно.
- Factory reset: любой источник постит `FACTORY_RESET` (WS MsgType 0x0008,
  позже — кнопка) → ConfigStore::reset() → дефолты + unlink(config.json) + esp_restart().

---

## STEP-01 — Ядро реестра (components/01_core/common)

Новые файлы:
- `include/AppTypes.h` — `fnv1a32`/`fieldUid`, `FixedString[32]`, макросы
  `CFG_STRING/INT/UINT/FLOAT/ENUM/IP/BOOL`, `enum class FieldDomain {WIFI,TWAI,SNAPSHOT,SYSTEM,PROTOCOL}`.
- `include/DataFields.inc` — обёртка `DATA_FIELD_DOMAIN(...)`.
- `include/fields/WifiFields.inc`, `fields/TwaiFields.inc`,
  `fields/SnapshotFields.inc`, `fields/SystemFields.inc` — таблицы `DATA_FIELD(...)`
  + аннотации `@label/@description/@group/@unit/@sensitive`.
- `include/AppData.h` — X-macro: `struct AppData`, `initAppDataDefault()`,
  `g_fieldMeta[]`, UID-константы (`apSsid_UID` и т.д.), `static_assert(checkFieldUids())`,
  размерные constexpr. (Образец: TEMP_PID `AppData.h`.)
- `include/FieldRegistry.h` + `src/FieldRegistry.cpp` — `getMetaByUid/ByName`,
  `readField/writeField/writeFieldScalar/fieldCount/fieldAt`, сериализация по §2,
  контроль владения readonly. (Образец: TEMP_PID `FieldRegistry.cpp`; PID-специфику
  про pidSetpoint/датчик — выкинуть.)

Изменения:
- `include/AppContext.h`: заменить `AppConfig config` → `AppData adata` +
  `std::recursive_mutex adataMutex` + `FieldRegistry fields` + RAII `AppDataLock`
  (образец: TEMP_PID `AppContext.h`). Удалить `include/AppConfig.h`.
- `CMakeLists.txt`: добавить `src/FieldRegistry.cpp`.
- Заменить `ctx->config.X` → `ctx->adata.X` в: `ConfigStore.cpp`, `WifiApModule.cpp:79-87`,
  `J1939System.cpp:111,144`.

Проверка: `idf.py reconfigure && idf.py build`.

## STEP-02 — ConfigStore на реестре + factory reset (components/05_storage/config_store)
- Убрать ручной парсинг. `buildFieldsJson()` — JSON только isConfig-полей по имени.
  `applyFieldsJson()` — по `getMetaByName`, unknown → дефолт (игнор).
  `applyFieldsWithNotify()` — для изменившихся UID: CONFIG_CHANGED + PUSH.
- Автосейв по дебаунсу оставить; `dirty_` теперь выставляется по подписке на CONFIG_CHANGED.
- `reset()`: подписка на FACTORY_RESET → initAppDataDefault (config-поля) →
  `unlink(config.json)` + `unlink(config.json.tmp)` → `esp_restart()`.
  (Образец: TEMP_PID ConfigStore.cpp:470-511.)
- `setAndSave()` удалить.
- Сохранение: атомарная запись tmp+fsync+rename (оставить как есть).

Проверка: сборка.

## STEP-03 — Протокол: MsgType + CommModule (components/04_network/communication, 01_core/common)
- `J1939Proto.h`: добавить 6 констант kMsgType* (0x0003..0x0008).
- `AppEvents.h`: `communication_send_event_t{uint16_t uid; int sockfd;}`, событие
  `COMMUNICATION_SEND`; событие `FACTORY_RESET`; `config_changed_event_t{int field}`
  → `field_change_event_t{uint16_t uid}`.
- `CommModule.cpp`:
  - `onIncomingPacket`: диспатч PARAM_REQUEST/PARAM_SET/FACTORY_RESET;
    PARAM_SET → `writeField(uid, payload+2, len-2, FieldDomain::PROTOCOL)`,
    diff → CONFIG_CHANGED + broadcast PUSH, ответ ACK/NACK.
  - подписка на COMMUNICATION_SEND → `sendField(uid, sockfd)` (wrapFrame(PARAM_PUSH) → WS_MESSAGE_SEND).
  - `onWsClientConnected` → push-on-connect: PUSH всех полей адресно сокету (замена лога).
  - `onWifiStatus` — оставить лог (WIFI-поля публикует WifiApModule).

Проверка: сборка.

## STEP-04 — TWAI runtime + конфиг (components/02_hardware/twai, 03_systems/j1939_system)
- `TwaiDriver.h/.cpp`: `struct Status { int state; uint32_t txErr, rxErr; };`
  `bool getStatus(Status&) const` через `twai_node_get_info`. Чистый драйвер, без AppContext.
- `J1939System`:
  - `begin()`: pins из HardwareConfig (оставить константами), `canBitrate` из `ctx->adata`
    (дефолт 250000).
  - `onJ1939Request`: src в CAN ID = `ctx->adata.canNodeAddr` (сейчас 0xFF);
    `Hw::kJ1939MyAddr` удалить из HardwareConfig.h.
  - подписка CONFIG_CHANGED: canNodeAddr/canTxTimeoutMs — применить сразу;
    canBitrate — лог «после перезагрузки», при записи валидировать в
    {125000,250000,500000,1000000}, иначе откат/отказ.
  - в taskLoop каждую ~1 с: `twai_.getStatus()` → writeFieldScalar twai* +
    COMMUNICATION_SEND; при `canAutoRecover && BUS_OFF` → `twai_.recover()` +
    twaiRecoverCount++. `activePgns` = `acc_.count()`.
  - timeout в `twai_.transmit(...)` брать из `canTxTimeoutMs`.

Проверка: сборка.

## STEP-05 — WifiApModule: live-apply + runtime-поля (components/04_network/wifi)
- Вынести в `applyConfig()`: сборку `wifi_config_t` и настройку IP (парсинг apIp
  через `sscanf("%d.%d.%d.%d")`).
- Подписка CONFIG_CHANGED: смена WIFI-конфига → `esp_wifi_set_config(WIFI_IF_AP,…)`;
  смена IP → `esp_netif_dhcps_stop/set_ip_info/dhcps_start` + пересоздать
  `DnsServer(новый IP)`; затем `esp_wifi_restart()` (кратковременный обрыв,
  клиенты переподключатся).
- `onEvent` (STACONNECTED/DISCONNECTED): writeFieldScalar wifiClients/wifiApMode +
  COMMUNICATION_SEND.

Проверка: сборка.

## STEP-06 — SystemStatusModule (опционально, новый components/03_systems/system_status)
- Маленький модуль (приоритет ~50): задача раз в 1 с пишет fwVersion/uptimeMs/heapFree
  (SYSTEM-домен) + COMMUNICATION_SEND. Зарегистрировать в `main/main.cpp`
  (`REGISTER_MODULE`). Можно выкинуть, если не нужен.

Проверка: сборка.

## STEP-07 — Документация
- `docs/PROTOCOL-J1939.md`: §2 — новые MsgType; §4 — param-payload (UID + сериализация
  значения); сценарии push-on-connect / SET / REQUEST / factory reset; примечание про
  config.json по именам полей и сброс к дефолтам.
- `AGENTS.md`: упомянуть реестр полей, замену AppConfig → AppData/FieldRegistry.

Проверка: не требуется (только документы), но собрать всё равно для уверенности.

## STEP-08 — Финальная проверка
- `idf.py reconfigure && idf.py build` (единственная доступная проверка).
- Проверить, что чеклист полностью `[x]`.

---

## Вне объёма (отдельные этапы)
- Фронтенд: пересборка под новые MsgType + реестр (перенос gen_shema.ts из TEMP_PID).
- Address-claim J1939 — только фиксированный конфигурируемый адрес.
- canTxGpio/canRxGpio — остаются compile-time.

## Риски / нюансы
- Live-рестарт AP рвёт WS на ~1 с (неизбежно при смене SSID/пароля).
- maxTrackedPgns — потолок 128 (массив в SnapshotAccumulator), поэтому max в реестре = 128.
- adataMutex обязателен: поля пишутся из event-loop (CommModule) и задач
  (J1939System/SystemStatus) параллельно.
- EventManager pool 64: +4 подписки — запас есть.
- Комментарии в проекте — на русском (стиль AGENTS.md), не удалять существующие.
- `idf.py build` — единственная проверка; тестов и линтера нет.