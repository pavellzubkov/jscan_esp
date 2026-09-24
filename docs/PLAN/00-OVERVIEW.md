# План рефакторинга jscan_esp → архитектура TEMP_PID

> Статус: **done** (STEP-01..08 выполнены, проект на новой архитектуре).
> Миграция инкрементальная: каждый шаг — отдельный
> исполняемый контекст, проект собирается на каждом шаге. Старые плоские
> компоненты удаляются только в STEP-07.

## 1. Цель

Перевести jscan_esp (сканер J1939, ESP-IDF 6.1) с плоской структуры
(`_common`/`esp_j1939`/`esp_wifi_start`/`esp_fs_start`) на компонентно-слоевую
модель проекта TEMP_PID (`E:\Projects\Embedded\ESP32\Temp_pid\TEMP_PID`):

- нумерованные слои `components/01_core..05_storage`;
- центральный **event bus** (AppContext + EventManager + AppEvents) — модули
  общаются только через события;
- **BootManager** — табличный загрузчик модулей с приоритетами;
- «глупые» драйверы железа (02_hardware) не знают об AppContext;
- координатор-система (03_systems) владеет драйверами и оркеструет;
- протокол по `docs/PROTOCOL_J1939_CHECKPOINT.md`: WS binary, кадр
  `magic/ver/flags/MsgType/len/seq/CRC`, батч снапшота `{SA,PGN,data,periodMs}`
  раз в 200–500 мс; SPN-декод делает фронтенд.

Фронтенд (`spiffs_image/`) на этом этапе **не трогаем** — он будет
пересобран позже под новый протокол.

## 2. Принятые решения (согласовано)

1. **Миграция инкрементальная** — новые слои добавляются рядом со старыми
   (проект собирается после каждого шага), переключение `main` и удаление
   старых компонентов — в STEP-07.
2. **05_storage включаем сразу** — `spiffs_service` + `config_store`
   (JSON на SPIFFS: AP-параметры, интервал снапшота). Реестр полей
   (AppData/DataFields из TEMP_PID) **не переносим** — под динамические PGN
   он не подходит.
3. **Командный канал RQST сохраняем** — кадр `MsgType=0x0002 J1939_REQUEST`
   от клиента → ESP шлёт RQST (PGN 59904) для запроса PGN 65227/65228.

## 3. Целевая структура

```
CMakeLists.txt                        — EXTRA_COMPONENT_DIRS: components/01_core..05_storage
main/main.cpp                         — AppContext + BootManager + REGISTER_MODULE
main/CMakeLists.txt                   — spiffs image из spiffs_image (без изменений фронта)
partitions_new.csv                    — оставить (factory 1MB + storage spiffs 1MB)
spiffs_image/                         — СБОРОЧНЫЙ деплой фронта, не редактировать

components/
  01_core/common/
    include/AppContext.h              — event_loop + EventManager + AppConfig
    include/AppEvents.h               — события + ws_message_t + j1939_*_event_t
    include/EventManager.h            — копия из TEMP_PID (без изменений)
    include/BootManager.h + src/      — копия из TEMP_PID
    include/SystemTiming.h            — тайминги (интервал снапшота и т.п.)
    include/HardwareConfig.h          — пины TWAI, MY_ADR, битрейт
    include/AppConfig.h               — struct AppConfig (дефолты конфига)
    include/J1939Proto.h + src/       — кадр + CRC16 + сериализация батча (чистые функции)
  02_hardware/twai/
    include/TwaiDriver.h + src/       — «глупый» драйвер TWAI (без AppContext)
  03_systems/j1939_system/
    include/J1939Decoder.h            — 29-бит CAN ID → PGN/SA/priority
    include/J1939TransportProtocol.h  — BAM + DT реасемблер (до 1785 байт)
    include/SnapshotAccumulator.h     — карта (SA,PGN) → {data, lastTs, periodMs}
    include/J1939System.h             — координатор: 1 задача rx + снапшот-таймер
  04_network/wifi/
    include/WifiApModule.hpp + src/   — AP-режим, публикация WIFI_STATUS
  04_network/server/
    include/ServerModule.hpp + src/   — httpd + static (SPIFFS) + WsHandler
    src/StaticHandler.cpp, WsHandler.cpp, WsHandler.hpp
  04_network/communication/
    include/CommModule.h + src/       — кадр/CRC/диспатч, подписка на снапшоты
  04_network/netctrl/
    include/NetworkController.hpp + src/ — владеет wifi + server
  05_storage/spiffs_service/
    include/SpiffsService.hpp + src/  — обёртка esp_spiffs (mount/read/write/exists)
  05_storage/config_store/
    include/ConfigStore.h + src/      — JSON config.json, дебаунс save, CONFIG_CHANGED
```

## 4. Карта переноса (старое → новое)

| Старое | Новое | Примечание |
|---|---|---|
| `_common/app_common.h` — `AppState`, `J1939Msg*`, `wsSendBytes`, enum'ы WS | `01_core/common` | `AppState` упразднён; `ws_message_t` переехал в AppEvents.h; `J1939Msg/Short` → `SnapshotRecord`/`TwaiRxFrame` |
| `_common` — макросы `bitset/bitclear/MAX/MIN` | `01_core/common` (J1939Proto/хелперы) | при необходимости перенести, не тащить мусор |
| `esp_j1939/j1939twai.cpp` — TWAI node, ISR-слоты, rx-таск, TP BAM/DT, requestPGN | `02_hardware/twai` + `03_systems/j1939_system` | драйвер отделён от логики декодирования/TP/аккумулятора |
| `esp_wifi_start/esp_wifi_start.cpp` — softAP | `04_network/wifi/WifiApModule` | конфиг (ssid/pass/channel) из `AppConfig` |
| `esp_fs_start/_fs_spiffs_sd.cpp` — init SPIFFS | `05_storage/spiffs_service` | + read/write/exists |
| `esp_fs_start/_fs_server.cpp` — HTTP static + REST | `04_network/server/StaticHandler` | глобальный `/*` GET-хендлер |
| `esp_fs_start/_ws_server.cpp` — WS + очередь→broadcast | `04_network/server/WsHandler` | порт из TEMP_PID (реестр клиентов, post WS_MESSAGE_RECEIVED) |
| `esp_fs_start/esp_fs_start.cpp` — фасад RunSPIFFS/HTTP/REST | упразднён | логика в модулях |
| — | `04_network/communication` | кадр+CRC+батч, командный канал RQST |
| — | `04_network/netctrl/NetworkController` | владеет wifi+server (как TEMP_PID) |
| — | `05_storage/config_store` | конфиг: AP + интервал снапшота |
| `main/main.cpp` — линейный Run* | переписан: BootManager + REGISTER_MODULE | |

## 5. События (event bus, AppEvents.h)

| Событие | Публикует | Подписан | Данные |
|---|---|---|---|
| `J1939_SNAPSHOT_SEND` | J1939System (таск снапшотов, ~250 мс) | CommunicationModule | `j1939_snapshot_t{length, data[]}` — сырые байты батча (postSized) |
| `J1939_REQUEST` | CommunicationModule (разбор входящего кадра 0x0002) | J1939System (шлёт RQST 59904) | `j1939_request_t{dstAddr, pgn}` |
| `WS_MESSAGE_RECEIVED` | WsHandler (server) | CommunicationModule | `ws_message_t` |
| `WS_MESSAGE_SEND` | CommunicationModule | WsHandler (server) | `ws_message_t` |
| `WS_CLIENT_CONNECTED` / `WS_CLIENT_DISCONNECTED` | WsHandler | CommunicationModule (лог) | `ws_message_t` |
| `WIFI_STATUS` | WifiApModule | CommunicationModule (лог) | `wifi_status_event_t` |
| `CONFIG_CHANGED` | ConfigStore (после debounce save) | (задел под будущее) | `config_changed_event_t{field}` |

Правило владения: `ctx->config` пишет **только** ConfigStore (load/defaults).
Остальные модули конфиг читают.

## 6. Модули BootManager

| Имя | Тип | Prio | critical | Смысл |
|---|---|---|---|---|
| `config` | ConfigStore | 10 | true | критичен: конфиг нужен всем; монтирует SPIFFS |
| `comm` | CommunicationModule | 20 | false | протокол/кадры/команды |
| `netctrl` | NetworkController | 40 | false | владеет wifi + server (HTTP/WS/static) |
| `j1939` | J1939System | 70 | false | TWAI-приём, TP, снапшоты |

## 7. Протокол

Полная спецификация — **`docs/PROTOCOL-J1939.md`** (создаётся вместе с планами).
Кратко: WS binary, кадр
`magic 0x5A A5 | ver=1 | flags | MsgType(2) | PayloadLen(2) | Seq(2) | payload | CRC16`;
батч `{count, (sa u8, pgn u32 LE, len u8, data[len], periodMs u16 LE)*}`;
`MsgType`: `0x0001 J1939_SNAPSHOT`, `0x0002 J1939_REQUEST`.
CRC16-CCITT-FALSE, покрытие `[0..len-3]`, LE. Реализация — в `J1939Proto` (01_core).

## 8. Лимиты ресурсов (жёсткие потолки)

| Ресурс | Лимит | Где |
|---|---|---|
| Подписки EventManager | `kMaxSubscriptions = 64` | EventManager.h |
| Очередь event loop | 256 | AppContext.h |
| Максимальный WS-фрейм | `kMaxWsMessageLen = 8192` | AppEvents.h |
| Максимальный батч-пейлоад | `kMaxBatchPayload = 8192` | AppEvents.h |
| Записей в аккумуляторе | `SnapshotAccumulator::kMaxRecords = 128` | SnapshotAccumulator.h |
| Макс. длина PGN-данных | `kJ1939MaxDataLen = 1785` | J1939Proto.h |
| TWAI RX-слотов | 8 | TwaiDriver (Config::rxSlots) |
| TWAI TX-очередь | 4 | TwaiDriver (Config::txQueueDepth) |
| Параллельных TP-сессий | 2 | J1939TransportProtocol |
| Модулей BootManager | `kMaxModules = 16` | BootManager.h |

## 9. Что вырезаем (не переносим)

- `AppState` и передачу указателя на весь стейт в каждый модуль;
- `MESS_WORK`, `MESS_SET_SPEED`, `MESS_SET_GEAR`, `WorkReg` — неиспользуемые
  заглушки (новый фронт пересоберём под новый протокол);
- прямой WS-broadcast сырых `J1939MsgShort`/`J1939Msg` — заменяется
  батч-фреймами снапшота;
- реестр полей AppData/DataFields/FieldRegistry из TEMP_PID — не подходит
  под динамические PGN (нет имён для UID-хэша);
- `WEB_CONFIG_FILENAME`/`InitConfig` из старого `_common` — конфиг теперь в
  `config_store`.

## 10. Порядок шагов (каждый — отдельный контекст)

| Шаг | Файл | Что делает |
|---|---|---|
| 1 | `01-core-common.md` | скелет слоёв (EXTRA_COMPONENT_DIRS) + `01_core/common` |
| 2 | `02-twai-driver.md` | `02_hardware/twai` |
| 3 | `03-j1939-system.md` | декодер + TP + аккумулятор + J1939System |
| 4 | `04-storage.md` | `05_storage` (spiffs_service + config_store) |
| 5 | `05-network-wifi-server.md` | `04_network/wifi` + `server` |
| 6 | `06-communication.md` | `04_network/communication` |
| 7 | `07-netctrl-main.md` | `netctrl` + переписывание main + удаление старых компонентов |
| 8 | `08-verify-finalize.md` | build+flash+monitor, WS-проверка снапшотов, README |

## 11. Источники-образцы

- Архитектура: `E:\Projects\Embedded\ESP32\Temp_pid\TEMP_PID\README.md`
  (слои, event bus, BootManager, WsHandler, коммуникация).
- Код для порта: `TEMP_PID\components\01_core\common\include\EventManager.h`,
  `BootManager.h/.cpp`, `AppContext.h`, `AppEvents.h`;
  `TEMP_PID\components\04_network\server\src\WsHandler.cpp/.hpp`,
  `ServerModule.hpp`; `TEMP_PID\components\04_network\communication\src\CommModule.cpp`
  (таблица CRC16, паттерн кадра).
- Текущий рабочий код для переноса логики: `components/esp_j1939/j1939twai.cpp`,
  `esp_fs_start/*`, `esp_wifi_start/esp_wifi_start.cpp`.

## 12. Команды проверки на каждом шаге

```bash
idf.py reconfigure   # пересканирует граф зависимостей (экономит токены)
idf.py build         # единственная проверка (тестов/линтера нет)
```
Прошивка: `idf.py -p <COMx> flash monitor`.