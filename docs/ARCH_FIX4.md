# ARCH_FIX4 — план исправлений по итогам ревью архитектуры

Пункты 2–5 из ревью (надёжность/расширяемость/поддержка). Пункт 1 ревью
(безопасность: auth OTA/FACTORY_RESET, apPassword в push-on-connect,
`max_open_sockets`) — отдельная задача, **не входит** в этот план.

## Решения (закрыты при планировании)

- **canTxTimeoutMs** — удалить поле (мёртвое: TX везде `timeout=0`).
- **OTA_BEGIN/OTA_END** — удалить события (подписчиков нет).
- **seq** — извлекать, дропать дубли, логировать разрывы с resync.
- **UID полей** — golden-тест «имя → uid» в host-тестах.

## Верификация (на каждом шаге и в финале)

```
cmake -S tests/host -B build/host && cmake --build build/host && ./build/host/host_tests
idf.py reconfigure && idf.py build
```

После шага 9: `grep -r "canTxTimeoutMs\|OTA_BEGIN\|subscribeDefault" components main tests` — 0 совпадений.

## Порядок работы с каждым шагом (обязателен)

После **каждого** шага выполнять строго по цепочке, затем **останавливаться**
и ждать подтверждения перед следующим шагом:

1. **Сборка** — `idf.py reconfigure && idf.py build` (чистая, без ошибок).
2. **Тесты** — host-тесты, если шаг меняет код из host-сборки
   (01/03-домены, J1939Proto, FieldRegistry, очередь, TP);
   для чисто рантайм-шагов (2, 5, 6, 7) достаточно сборки,
   но при наличии затронутых host-тестов — прогнать их обязательно.
3. **Коммит** — один коммит на шаг, сообщение вида
   `arch-fix4: шаг N — <краткое описание>`; в коммит включать только
   изменения этого шага (+ тесты к нему).
4. **Пуш** — `git push` сразу после коммита.
5. **Отметка в доке** — в `docs/ARCH_FIX4.md` поставить `[x]` на все
   пункты выполненного шага и в разделе «Статус»; включить эту правку
   в коммит шага (или отдельным `DOCS:`-коммитом сразу после него).
6. **Стоп** — не начинать следующий шаг без отдельного разрешения.

Финал (после шага 9): host-тесты зелёные + чистый `idf.py build` +
grep-проверка, затем коммит/пуш и полная остановка.

## Порядок коммитов

Шаги 1 → 3 → 4 → 8 (тесто-ориентированные) → 2 → 5 → 6 → 7 → 9 (правки
рантайма; 9 последним, чтобы golden-тест и тесты proto были свежими).

Между любыми двумя шагами — обязательная остановка (см. «Порядок работы
с каждым шагом»): сборка → тесты → коммит → пуш → стоп.

---

## Шаг 1. Decoder: PDU1-границы + фильтр std/RTR-кадров

**Баг:** `peerToPeer` (`J1939Decoder.cpp:3-11`) — `pgn=0` (PF=0, dst=0) и
`0x10000` (DP=1, PF=0) классифицируются как broadcast; тесты
`tests/host/main.cpp:513-519` закрепили баг (`CHECK(!peerToPeer(0))`).

- [x] `J1939Decoder.cpp`: `peerToPeer` → `return ((pgn >> 8) & 0xFF) < 0xF0;`
      (критерий PF<240, покрывает все страницы DP/R).
- [x] `decode` (`:24-34`): PDU1 → `dst = pgnRaw & 0xFF`, чистый
      `pgn = pgnRaw & 0x3FFF00` (PS→0, R/DP/PF сохраняются — сейчас маска
      `0x1FF00` дополнительно срезает R); PDU2 → `dst=0xFF`, `pgn=pgnRaw`
      (PS — часть PGN, не трогать).
- [x] `TwaiDriver.cpp:20-54` (`rxDoneCb`): после `twai_node_receive_from_isr`
      отбрасывать кадры с `header.ide==0` или `rtr!=0` (вернуть слот в free) —
      проект 29-bit data-only; RxFrame становится доверенным для декодера.
- [x] Тесты (`main.cpp:487-520`): инвертировать границы
      (`peerToPeer(0)==true`, `peerToPeer(0x10000)==true`), добавить
      decode-кейс PF=0/PS=0 → `isP2P && dst==0 && pgn==0`,
      PDU2 → `dst==0xFF`.

## Шаг 2. TWAI: on_error, bus-off backoff, лимит ретраев

В IDF 6.1 есть `twai_event_callbacks_t::on_error` (`esp_twai_types.h:135`,
`twai_error_event_data_t` c `old_sta/new_sta/err_flags`) — подписка вместо
поллинга раз в секунду.

- [x] `TwaiDriver.hpp`:
  - `Config += int failRetryCnt = 3;` (сейчас хардкод `-1` в
    `TwaiDriver.cpp:152`; диапазон IDF [-1:15] — бесконечная ретрансляция
    иссушает TX-пул на мёртвой шине);
  - `static bool stateChangeCb(...)` (колбэк драйвера; **on_state_change**,
    не on_error — on_error несёт только err_flags, old_sta/new_sta в
    state_change): при `new_sta == BUS_OFF`
    → `busOffSeen_.store(true)`, `busOffEvents_++` (атомики — контекст
    колбэка неизвестен);
  - `bool takeBusOffEvent()` — `exchange(false)`.
- [x] `TwaiDriver.cpp`: `cbs.on_state_change = stateChangeCb` рядом с `on_rx_done`.
- [x] `J1939System`:
  - вынесен recover из `updateTwaiStatus` в
    `tryRecoverBusOff(nowMs)`; вызывается из `taskLoop` по
    `takeBusOffEvent()` + fallback в тик телеметрии;
  - **backoff**: `kRecoverBackoffBaseMs=100`, ×2 до cap 30 с; таймер
    `nextRecoverAllowedMs_`; backoff сбрасывается, если после recover
    прошло >60 с без нового bus-off. Счётчик `twaiRecoverCount_` остаётся;
  - публикация `twaiState` (RUNNING/BUS_OFF/RECOVERING) без изменений.
- Безопасность: `recover()` уже под `txMux_` — не трогаем; уважать
  `canAutoRecover` (как сейчас).

## Шаг 3. Вынос сериализации TP.CM + тест байтов CTS/EOM

Сборка кадра инкапсулирована в `J1939System::sendTpAction` (`:425-470`) —
J1939System не входит в host-сборку, байты не проверены.

- [x] `J1939TransportProtocol.hpp/cpp` (слой 03, уже в host-тестах) — две
      статики:
  - `buildCmPayload(const TpAction&, uint8_t out[8])` — тело `sendTpAction`
    (memset 0xFF, CTS `[17, n, 0xFF…]`, EOM `[19, len LE, packets, 0xFF, …]`,
    PGN LE в байты 5–7);
  - `buildCmId(const TpAction&, uint8_t nodeAddr)` —
    `(7<<26) | (Hw::kPgnTpCm<<8) | (dst<<8) | sa` (HardwareConfig доступен:
    03→01).
- [x] `J1939System::sendTpAction` → вызовы этих функций + `twai_.transmit`.
- [x] Тесты: побайтовая сверка CTS/EOM payload и ID (включая `act.dst` в PS),
      `Kind::None` → без кадра.

## Шаг 4. Тест J1939MsgChannel + реализация стаба очереди

`tests/host/stubs/freertos/queue.h` — пустышка (4 строки): нужна минимальная
реализация. Контракт владения `bigData` сейчас не покрыт вовсе.

- [x] Стаб `queue.h`: header-only кольцевая очередь
      (`xQueueCreate/xQueueSend/xQueueReceive/vQueueDelete`); ненулевой
      `ticks` → мгновенный `pdFALSE` (блокировок в тестах не нужно —
      задокументировать в стабе).
- [x] Тесты:
  - roundtrip inline `len<=8` (поля, `bigData==nullptr`);
  - roundtrip `len>8` (буфер переходит через `pop`, `free` в тесте);
  - `setConsumer(false)` → `push==false`, `drops_++`;
  - overflow: 33-й `push` → дроп + счётчик, данные первых 32 не искажены;
  - `drain` в dtor не падает.

## Шаг 5. BootManager: destroy при critical-отказе + код ошибки в main

- [x] `BootManager.hpp`: `Entry += void* instance; void (*destroy)(void*);`;
      сигнатура `init` → `(AppContext*, Entry&)`; `makeModule` получает
      `void** outInst` (заполняет при успехе); `REGISTER_MODULE` в лямбде
      ставит `e.instance/e.destroy`
      (`destroy = [](void* p){ delete static_cast<Type*>(p); }`).
- [x] `BootManager.cpp:57-59` (critical-ветка): **перед** `return` —
      `dumpStatus()` + останов уже стартовавших в обратном порядке
      (`destroy(instance)`, сброс `started_[j]`); упавший модуль уже удалён
      `makeModule` — его не трогаем.
- [x] `main.cpp:41-44`: лог `esp_err_to_name(overall)` вместо безликого
      `ESP_LOGW`.
- [x] `main.cpp:51`: проверить return `esp_ota_mark_app_valid_cancel_rollback()`.
- Деструкторы модулей уже корректны (graceful shutdown ARCH_FIX2) —
  destroy = готовая остановка.

## Шаг 6. Rate-limit входящих WS-кадров

- [x] `WsHandler.hpp`: `kMaxInboundFramesPerSec = 50`; слоты
      `{sockfd, windowMs, count}` на `kMaxClients` (обработчики httpd — одна
      задача, без лока; сброс слота в `add_client`/`remove_client`).
- [x] `WsHandler.cpp:275-307` (после валидации размера, до `malloc/post`):
      превышение окна → дроп + новая атомика `rxDrops_` + rate-limited лог
      (первый и каждый 50-й). Соединение не закрываем — только теряем кадры.
      Дополнительно: сброс всех слотов в `reg()` (рестарт httpd) и `rxDrops_`
      в логе `unreg()`.
- Окно — `Timing::nowMs()` (`SystemTiming.hpp`, 01→04 разрешено).

## Шаг 7. seq: извлечение + дроп дублей в CommModule

- [x] `J1939Proto::unwrapFrame` (`:118-139`): out-параметр `uint16_t* seq`
      (байты 8–9, LE); обновить call site `CommModule.cpp:78` и тесты
      (roundtrip seq через `wrapFrame`).
- [x] `CommModule`: состояние на sockfd `{sockfd, lastSeq, valid}` — массив,
      обработка только в event-loop (одна задача — без локов):
  - `WS_CLIENT_CONNECTED` → сброс слота (реконнект — новая серия);
  - `seq==last && valid` → **дроп** + лог;
  - иначе `lastSeq=seq`; если не `last+1` — лог разрыва (resync, кадр
    принимаем).
- [x] `docs/PROTOCOL-J1939.md`: задокументировать семантику seq (в финальной
      ревизии спеки байт seq не описан).

## Шаг 8. Golden-тест стабильности UID

- [x] `tests/host/main.cpp`: захардкоженная таблица `{имя, uid}` **всех**
      полей (генерация: временный вывод `g_fieldMeta` → вручную в тест);
      двусторонняя сверка с `g_fieldMeta` — переименование/удаление/добавление
      поля без обновления таблицы = красный тест.
- [x] Отдельный `test_field_uid_stability()`, вызов в `main()`.

## Шаг 9. Удаление мёртвого + унификация updateField

### 9a. canTxTimeoutMs → удалить

- [x] `TwaiFields.inc:16` (поле), `HardwareConfig.hpp:27`
      (`kDefaultTxTimeoutMs`);
- [x] `J1939System.cpp:208-219` (чтение+лог в `sendRequest` — остаётся
      `ESP_LOGW` про queue full без упоминания поля), `:241-248` (case),
      комменты `J1939System.hpp:40,60`;
- [x] `tests/host/main.cpp:174` (CHECK убрать) + golden-таблица.

### 9b. OTA_BEGIN/END → удалить

- [x] `AppEvents.hpp:29-30` (enum), `:151-160` (traits);
- [x] `OtaService.cpp:76-84` (`enterOta/exitOta`) — убрать методы и их
      вызовы (grep при исполнении), комменты `OtaService.hpp:48-50`.

### 9c. EventManager::subscribeDefault → удалить

- [x] `EventManager.hpp:232-263` (0 вызовов).

### 9d. updateField → обёртка над writeFieldDetectChange

- [x] `LogicUtils.hpp:24-49` — убрать свой read/write/read; брать домен из
      meta:

```cpp
const FieldMeta* m = ctx->fields.getMetaByUid(uid);
bool changed = false;
auto st = ctx->fields.writeFieldDetectChange(uid, &value, sizeof(T),
                                             m->domain, &changed);
// st != OK → лог; changed → sendField
```

Семантика чище: `READONLY_DENIED`/`OUT_OF_RANGE` возвращаются наружу
(раньше глушились). Call sites (`J1939System`, `J1939Scanner`,
`WifiApModule`) не меняются — сигнатурная совместимость.

---

## Статус

- [x] Шаг 1 — decoder PDU1 + фильтр ide/RTR
- [x] Шаг 2 — TWAI on_error + bus-off backoff + fail_retry_cnt
- [x] Шаг 3 — сериализация TP.CM + тесты CTS/EOM
- [x] Шаг 4 — тест J1939MsgChannel + стаб очереди
- [x] Шаг 5 — BootManager destroy + main
- [x] Шаг 6 — rate-limit входящих WS
- [x] Шаг 7 — seq дроп дублей
- [x] Шаг 8 — golden-тест UID
- [x] Шаг 9 — удаление мёртвого + updateField
- [ ] Финал: host-тесты зелёные + чистый `idf.py build`