# План починки дефектов архитектурного ревью (ARCH_FIX)

Источник: ревью надёжности/поддерживаемости/расширяемости (октябрь 2026).
Безопасность (FACTORY_RESET/OTA/WS-auth) — **вне** этого плана.

## Как работать с этим планом

- **Один шаг = одна сессия.** Каждый шаг самодостаточен: содержит цель,
  файлы с путями, конкретные правки и подводные камни. Начинай новый шаг
  только после того, как предыдущий собран и пройден, и только по явной
  команде пользователя.
- **После каждого шага — сборка, затем СТОП.** Не накапливай изменения
  между шагами: если сборка упала — чини в рамках текущего шага и
  собирайся заново, прежде чем двигаться дальше.
- **Выполненные шаги помечать**: заголовок шага — «## Шаг N. … ✔»
  (или `[x]` в нумерации), строка в сводной таблице — ☑. Пометка
  ставится только после успешной проверки (сборка + тесты, если есть).
- Семантика «СТОП» в конце шага: выведи результат проверки пользователю
  и прекрати работу до новой команды.

## Команды проверки (используются во всех шагах)

Прошивка (ESP-IDF 6.1, из корня проекта):

```bat
idf.py build
```

Host-тесты — **нативно в WSL** (Ubuntu-22.04, g++/cmake установлены;
проверено: `checks=68 failures=0`). Из PowerShell:

```powershell
wsl -d Ubuntu-22.04 -- bash -lc "cd /mnt/e/Projects/Embedded/ESP32/J1939_scaner/jscan_esp && cmake -S tests/host -B build/host && cmake --build build/host -j4 && ./build/host/host_tests"
```

(Путь `/mnt/e/...` — корень проекта в WSL. Успешный вывод:
`checks=N failures=0`.)

Если менялся состав файлов `tests/host/CMakeLists.txt` — удали
`build/host` перед пересборкой (`wsl ... rm -rf build/host`).

---

## Шаг 1. SnapshotAccumulator: UAF дубликатов + TTL-инвалидация ✔

**Цель.** Устранить use-after-free/double-free в аккумуляторе PGN и утечку
протухших записей.

**Файлы.**
- `components/03_systems/j1939_system/src/SnapshotAccumulator.cpp`
- `components/03_systems/j1939_system/include/SnapshotAccumulator.h`
- `tests/host/main.cpp`

**Дефекты.**
1. `collect()` (`SnapshotAccumulator.cpp:108-124`) уплотняет записи
   копированием `records_[n] = records_[i]`, **не очищая исходный слот**.
   Живут два валидных слота с общим указателем `bigData`. Далее:
   - вытеснение (`update()`, `:83-86`) делает `free(victim.bigData)` для
     одного дубликата — второй остаётся висячим → **UAF** при
     сериализации батча;
   - `storeData()` (`:56`) освобождает/перераспределяет общий указатель
     → **double-free** в худшем случае.
2. Протухшие записи в `collect()` просто пропускаются (`:117-118`), но
   остаются `valid=true` → слоты и `bigData` не освобождаются никогда
   (пока не вытеснятся), `count_` завышает `activePgns`.

**Правки.**

1. `collect()` — после копирования записи в новый слот **переносить
   владение**, а не дублировать:
   ```cpp
   if (n != i) {
       records_[n] = r;
       records_[i].bigData = nullptr;   // владение перешло в слот n
       records_[i].valid = false;       // исходный слот свободен
   }
   ++n;
   ```
2. В том же цикле — инвалидация по TTL. Проверку `(nowMs - r.lastTsMs)
   > ttlMs` вынести **до** копирования; для протухшей записи:
   ```cpp
   if ((nowMs - r.lastTsMs) > ttlMs) {
       if (r.bigData) { free(r.bigData); records_[i].bigData = nullptr; }
       records_[i].valid = false;
       if (count_ > 0) --count_;
       continue;
   }
   ```
   (Сравнение уже корректно переполняется uint32 — как в `update`.)
3. После цикла хвост `[n, kMaxRecords)` гарантированно содержит только
   невалидные слоты (копирование очищает источник, TTL инвалидирует) —
   проверить, что это так; при сомнении в цикле очистить хвост явно.
4. `count_` теперь истинное число валидных слотов: в `update()` путь
   вытеснения (`wasValid`) уже не меняет `count_` — оставить как есть
   (слот victim остаётся валидным, `count_` не меняется — корректно).

**Подводные камни.**
- `Record` содержит `bigData` — копирование структуры обязано
  сопровождаться обнулением источника; никакого «копируем и забываем».
- `count_` не должен уходить ниже 0 и не должен расходиться с числом
  `valid`-слотов — иначе `activePgns` (runtime-поле, диапазон 0..128)
  начнёт отдавать OUT_OF_RANGE.

**Новые host-тесты** (в `test_snapshot_accumulator`, `tests/host/main.cpp`):
- большая запись (len>8) → `collect()` дважды подряд → `update()` того же
  (sa,pgn) с `len>8` → ещё `collect()` → проверить, что данные читаются и
  `n` корректен (регрессия UAF на ASan-уровне логики: владение одно);
- заполнить 128 слотов большими записями, `collect()`, вытеснить старую —
  `count()==128`, `n==128`;
- TTL: запись с `bigData`, `collect(now, ttl)` после истечения TTL →
  `n==0`, `count()==0`.

**Проверка.** Host-тесты (команда выше) → `idf.py build`.

**СТОП.** Шаг завершён: `checks=N failures=0` + сборка прошла.
Следующий шаг выполнять только по новой команде.

---

## Шаг 2. ConfigStore: OOB-read JSON + гонки dirty_/reset ✔

**Цель.** Убрать чтение за границей буфера при парсинге конфига и две
гонки записи.

**Файлы.**
- `components/05_storage/config_store/src/ConfigStore.cpp`

**Дефекты.**
1. `loadFromFs()` (`:246-260`): буфер ровно `st.st_size` **без NUL**,
   `cJSON_Parse(buf)` читает за границей (OOB-read; при совпадении мусора
   парс падает и конфиг молча теряется).
2. `saveToFs()` (`:411`): `dirty_.store(false)` **после** записи —
   изменение, пришедшее во время записи, затирается и в файл не попадает.
3. `reset()` (`:422`): `initAppDataDefault(ctx_->adata)` **без
   AppDataLock** — гонка с задачами WIFI/SYSTEM, пишущими adata.

**Правки.**
1. `loadFromFs()`: выделить `st.st_size + 1`, после чтения
   `buf[rd] = '\0'`, затем `cJSON_Parse(buf)` (самый простой и
   безопасный вариант; `cJSON_ParseWithLength` тоже допустим — тогда
   выделять ровно `st_size` и передавать `rd`).
2. `saveToFs()`: в самом начале (до сбора JSON) — `dirty_.exchange(false)`;
   в конце убрать `dirty_.store(false)`. Логика: сброс до записи, любое
   изменение во время записи снова поставит `dirty_` → автосейв повторится.
3. `reset()`: обернуть `initAppDataDefault(ctx_->adata)` в
   `AppDataLock dataLock(ctx_);` (мьютекс рекурсивный — вложенность
   безопасна; вызов идёт из event-loop задачи под контекстом).

**Подводный камень.** `loadFromFs()` вызывается в `begin()` до создания
автосейв-задачи — гонок там нет, правка только про OOB.

**Проверка.** `idf.py build` (host-тестов нет: файл зависит от IDF/LittleFS).

**СТОП.** Шаг завершён: сборка прошла. Следующий шаг — только по новой
команде.

---

## Шаг 3. EventManager: unsubscribe → type-confusion слотов ✔

**Цель.** Убрать латентную путаницу обработчиков при удалении модуля
(error-path `BootManager::makeModule` → `delete` → `~Module` →
`unsubscribe(this)`).

**Файлы.**
- `components/01_core/common/include/EventManager.h`

**Дефект.** `unsubscribe(obj)` (`:284-308`) при удалении слота
**перемещает в дыру последний элемент** пула (`subscriptions_[i] =
subscriptions_[count-1]`), но `handler_args`, переданный в
`esp_event_handler_instance_register_with` (`:99`, `:139`), указывает на
**адрес старого слота**. Следующий `reserveSubscription()` (`:55-67`)
возвращает слот по индексу `subscriptionCount_` — который теперь
совпадает с адресом, на который всё ещё ссылается чужой обработчик.
Переиспользование → лямбда-каст (`:95`, `:134`) вызовет обработчик
другого типа на чужом объекте.

**Правка (выбран вариант «слоты не двигаются»).**
1. Добавить в `Subscription` флаг `bool inUse = false;`.
2. `reserveSubscription()`: линейный поиск первого слота с
   `inUse == false` (вместо `&subscriptions_[subscriptionCount_]`);
   при успехе выставить `inUse = true`. Пул фиксированный (64) — поиск
   O(n) не критичен.
3. `subscribe(...)` (все 3 перегрузки + `subscribeDefault`): при ошибке
   `esp_event_handler_instance_register_with` сбрасывать `inUse =
   false` (освобождать слот) — иначе слот утекает.
4. `unsubscribe(obj)`: после unregister — **не перемещать** элементы;
   просто `s->inUse = false; s->instance = nullptr; s->obj = nullptr;
   s->method = nullptr; s->base = nullptr; s->id = 0;`.
5. `shutdown()`: итерировать все `kMaxSubscriptions` слотов и
   unregister’ить те, где `inUse && instance`; затем выставить все
   `inUse = false`. Убрать зависимость от `subscriptionCount_`
   (можно оставить счётчик только для логов, но корректность не должна
   от него зависеть).
6. Убрать/поправить комментарий `:40-41` («указатель на элемент пула»
   теперь гарантированно стабилен).

**Подводные камни.**
- `post()`/`postSized()` не трогают — они не зависят от слотов.
- Пул стал «дырчатым» — это нормально; порядок слотов больше не
  гарантируется, рассчитывать на него нельзя нигде.
- host-тестов нет (esp_event не подключён в tests/host) — проверка
  только сборкой.

**Проверка.** `idf.py build`.

**СТОП.** Шаг завершён: сборка прошла. Следующий шаг — только по новой
команде.

---

## Шаг 4. WsHandler: лимит control-frame + гонка client_count_ ✔

**Цель.** Закрыть безлимитный `malloc` на входящих WS-кадрах и чтение
`client_count_` вне лока.

**Файлы.**
- `components/04_network/server/src/WsHandler.cpp`
- `components/04_network/server/src/WsHandler.hpp`
- `components/01_core/common/include/AppEvents.h`

**Дефекты.**
1. `:159-175`: для PING/PONG/CLOSE `malloc(ws_pkt.len)` **без верхнего
   лимита** (в отличие от BINARY/TEXT на `:218`). Фрейм с огромной
   declared-длиной → исчерпание кучи (OOM-паттерн; RFC ограничивает
   control frame 125 Б, но извне это не проверяется).
2. `remove_client()` (`:84-86`): `client_count_` читается **вне**
   `taskENTER_CRITICAL` — гонка с `add_client()` из другой задачи.

**Правки.**
1. **Отдельная константа входящего лимита.** В `AppEvents.h` рядом с
   `kMaxWsMessageLen` (`:26`) завести самостоятельную константу, не
   привязанную семантически к батчу, например:
   ```cpp
   // Входящее WS-сообщение: лимит запросов клиентов (PARAM_SET/REQUEST).
   constexpr size_t kMaxWsInboundLen = 1024;
   ```
   В `WsHandler.cpp:218` заменить `kMaxWsMessageLen` → `kMaxWsInboundLen`
   (исходящие фреймы через этот путь не ходят — там свой путь
   `send_to_all_clients`). Если найдутся другие места, где
   `kMaxWsMessageLen` используется для входящих — тоже заменить.
2. В ветке PING/PONG/CLOSE (`:164`) — до `malloc` проверка:
   ```cpp
   if (ws_pkt.len > kMaxWsInboundLen) { ... вернуть ESP_ERR_INVALID_SIZE; }
   ```
   (Либо ограничить 125 Б как в RFC — допустимо; тогда комментарий, что
   это спецификация RFC 6455 §5.5.)
3. `remove_client()`: `int count_for_log;` — читать `client_count_`
   внутри критической секции в локальную переменную, логировать уже её
   (как сделано в `add_client` `:52` — `new_count`).

**Подводный камень.** `kMaxWsMessageLen` оставить экспортируемым — на
него могут ссылаться другие модули; не удалять, только переназначить
использование во входящих путях.

**Проверка.** `idf.py build`.

**СТОП.** Шаг завершён: сборка прошла. Следующий шаг — только по новой
команде.

---

## Шаг 5. FieldRegistry: корректная запись строк (fwVersion) ✔

**Цель.** Починить молчаливый отказ записи `FixedString`-полей через
`writeFieldScalar` и добавить хелпер записи строк.

**Файлы.**
- `components/01_core/common/include/FieldRegistry.h`
- `components/03_systems/system_status/src/SystemStatusModule.cpp`
- `tests/host/main.cpp`

**Дефект.** `SystemStatusModule::begin()` (`:33`):
`writeFieldScalar(fwVersion_UID, FixedString(Hw::kFwVersion))` —
`writeFieldScalar` передаёт в `writeField()` сырые 64 байта `FixedString`,
а `valueInRange`/`deserializeField` (`FieldRegistry.cpp:57-96`) ждут
wire-формат `{len u8, bytes}`. Первый байт строки (символ) интерпретируется
как длина → всегда `OUT_OF_RANGE`. Статус игнорируется. То же касается
любого `writeFieldScalar(uid, someFixedString)` и `updateField()` с
`FixedString` (`LogicUtils.h:32`).

**Правки.**
1. В `FieldRegistry.h` добавить метод:
   ```cpp
   // Запись строкового поля (CFG_STRING/CFG_IP/CFG_PASSWORD) из C-строки:
   // сама конструирует wire {len, bytes} и вызывает writeField с доменом поля.
   FieldWriteStatus writeFieldString(uint16_t uid, const char* value);
   ```
   Реализация в `FieldRegistry.cpp`: достать meta, проверить
   validator ∈ {CFG_STRING, CFG_IP, CFG_PASSWORD}, `strlen(value)` ≤ 255,
   собрать буфер `[1+strlen]`, вызвать `writeField(uid, buf, 1+len,
   meta->domain)`.
2. `SystemStatusModule.cpp:33`: заменить на
   `ctx_->fields.writeFieldString(fwVersion_UID, Hw::kFwVersion);`
   и **проверить статус**: при `!= OK` — `ESP_LOGE(TAG, ...)`.
   (Альтернатива, допустимая: `fwVersion` и так инициализируется
   дефолтом `initAppDataDefault` — строку `:33` можно удалить совсем;
   выбрать вариант с `writeFieldString`, чтобы хелпер был покрыт
   реальным вызовом.)
3. В `LogicUtils.h::updateField` (`:32`) ничего не менять — он вызывает
   `writeFieldScalar`; добавить в комментарий `:12-17`, что для
   `FixedString` использовать `writeFieldString` + `sendField` отдельно
   (или оставить как есть, если `updateField` со строками нигде не
   вызывается — проверить grep’ом перед правкой).

**Новые host-тесты** (в `test_field_registry`):
- `writeFieldString(apSsid_UID, "NEW_SSID") == OK`, затем `readField` →
  wire `{7,"NEW_SSID"}`;
- `writeFieldString(apPassword_UID, "1234567")` (7 символов) →
  `OUT_OF_RANGE`;
- запись строки в числовое поле (`canBitrate_UID`) → `BAD_LENGTH`
  (защита от неверного вызова).

**Проверка.** Host-тесты → `idf.py build`.

**СТОП.** Шаг завершён: `checks=N failures=0` + сборка прошла.
Следующий шаг — только по новой команде.

---

## Шаг 6. TwaiDriver: утечки error-path + TX-буфер

**Цель.** Убрать утечки очередей/слотов при ошибке `begin()` и сделать
ожидание передачи достоверным.

**Файлы.**
- `components/02_hardware/twai/src/TwaiDriver.cpp`

**Дефекты.**
1. `begin()` (`:39-99`): при ошибке `new RxSlot[...]` (`:53`) очереди
   `rxFreeQueue_`/`rxReadyQueue_` не удаляются; при ошибке
   `twai_node_register_event_callbacks`/`twai_node_enable` (`:83-96`)
   узел удаляется, но **очереди и слоты не освобождаются** (вызов
   `end()` не происходит — `begin` вернул ошибку, `J1939System::begin`
   тоже выйдет без `end()`).
2. `transmit()` (`:146-151`): фрейм на стеке; `twai_node_transmit`
   копирует? — по комментарию `:149` драйвер держит указатель до конца
   передачи. Результат `twai_node_transmit_wait_all_done` **игнорируется**
   → при таймауте стек-буфер может быть ещё в использовании драйвера
   (UAF), а TX-статус недостоверен.

**Правки.**
1. Вынести очистку в локальный helper/лямбду `cleanup_partial()`:
   `vQueueDelete` для обеих очередей (с обнулением), `delete[]
   slots_` (с обнулением) — вызывать на **каждом** error-path `begin()`.
   После очистки все хэндлы `nullptr`, чтобы `end()` оставался
   идемпотентным (он уже проверяет на nullptr).
2. `transmit()`: обернуть `twai_node_transmit_wait_all_done` в проверку
   результата. При таймауте/ошибке:
   - вернуть ошибку (`ESP_ERR_TIMEOUT`), чтобы вызывающий код
     (`J1939System::sendRequest`) знал, что кадр не доставлен;
   - **решение по буферу**: буфер `txData` на стеке остаётся допустимым
     только если драйвер гарантирует отвязку после возврата
     `wait_all_done` (в т.ч. по таймауту). Проверить комментарии API
     `esp_twai_onchip.h` в IDF 6.1 (`G:\ESPIDFv6.1\...`): если гарантии
     нет — перейти на `malloc` для `txData` и освобождать после
     успешного wait, а при таймауте — освобождать в `end()` (держать
     указатель pendingTxBuf_ в классе). Описать выбор в комментарии.
3. `sendRequest` в `J1939System.cpp:147` — результат `transmit` пока не
   обязательно обрабатывать (отдельная задача); минимально: лог `ESP_LOGW`
   при `!= ESP_OK`.

**Подводный камень.** ISR-путь (`rxDoneCb`) не трогать. `end()` должен
остаться корректным и после частичного `begin()`.

**Проверка.** `idf.py build`.

**СТОП.** Шаг завершён: сборка прошла. Следующий шаг — только по новой
команде.

---

## Шаг 7. WifiApModule: дебаунс live-apply + валидация IP

**Цель.** Один `esp_wifi_stop/start` на пачку изменений WIFI-полей вместо
N рестартов, и валидный IP из любого источника.

**Файлы.**
- `components/04_network/wifi/src/WifiApModule.cpp`
- `components/04_network/wifi/include/WifiApModule.hpp` (если есть —
  фактический путь уточнить glob’ом `components/04_network/wifi/**`)
- `components/01_core/common/include/AppEvents.h`
- `components/01_core/common/src/FieldRegistry.cpp` (валидация CFG_IP)
- `tests/host/main.cpp`

**Дефекты.**
1. `onConfigChanged` (`:182-215`): каждое изменённое поле
   (apSsid/apPassword/apChannel/maxStaConn/apIp) → отдельный
   `applyConfig + esp_wifi_stop + esp_wifi_start`. Пачка из 5 полей =
   5 рестартов AP и 5 отключений клиентов.
2. `parseIp` (`:55-62`): `sscanf("%d.%d.%d.%d")` не проверяет ни число
   октетов, ни диапазон (999 → 0xE7), ни мусор в хвосте.
3. Валидатор `CFG_IP` в `FieldRegistry.cpp` меряет только длину строки
   (`:59-66`) — «999.999.999.999» (15 символов) проходит.

**Правки.**
1. **Дебаунс через esp_timer → новое событие** (согласовано):
   - В `AppEvents.h` добавить `app_event_id_t::WIFI_REAPPLY` (без
     данных, комментарий «отложенное применение конфига AP»).
   - В `WifiApModule`: член `esp_timer_handle_t reapplyTimer_ = nullptr;`
     одноразовый таймер; создать в `begin()` (`esp_timer_create` с
     `.dispatch_method = ESP_TIMER_TASK`, `.skip_unhandled_events = true`),
     удалить в `stop()` (`esp_timer_delete`).
   - `onConfigChanged` (наш case WIFI-полей): вместо немедленного
     `applyConfig()+stop/start` — перезапустить таймер:
     `esp_timer_restart(reapplyTimer_, 500000)` (500 мс; если таймер не
     создан/ошибка — fallback: применить немедленно, как сейчас).
   - Колбэк таймера (runs in esp_timer task — **нельзя** звать wifi API
     напрямую): только
     `ctx_->events.post(APP_EVENTS_BASE, app_event_id_t::WIFI_REAPPLY);`
     — thread-safe путь через event-loop.
   - Подписаться в `begin()`: `subscribe(APP_EVENTS_BASE,
     WIFI_REAPPLY, &WifiApModule::onWifiReapply, this)`; в обработчике —
     текущая логика `applyConfig() + esp_wifi_stop + esp_wifi_start`
     (перенести тело из `onConfigChanged`).
   - Проверить, что подписка инициализируется один раз (unsubscribe в
     `~WifiApModule`/`stop` уже есть через `unsubscribe(this)` — убедиться,
     что он покрывает и новый обработчик; `stop()` не должен убивать
     таймер до `delete` в dtor — порядок: сначала `esp_timer_stop/
     delete`, потом wifi stop).
2. **Валидация IP** — в один слой валидации, `FieldRegistry.cpp`,
   ветка `CFG_IP` в `valueInRange` (`:59-66`): после проверки длины
   парсить строку: ровно 4 октета, каждый 0..255, только цифры и точки,
   нет пустых октетов. Удобно — статическая функция
   `static bool isValidIpv4(const char* s, size_t len)` рядом; `min=7,
   max=15` из meta оставить.
3. `parseIp()` (`:55-62`): после валидатора поле уже валидно, но
   parseIp дублирует разбор — сделать его строгим: `sscanf` с `%n` или
   ручной разбор с проверкой `a,b,c,d ∈ [0,255]` и ровно 4 октетов;
   при любой ошибке — вернуть дефолт `Hw::kApIp` + `ESP_LOGW`.

**Новые host-тесты** (в `test_field_registry`):
- `writeField(apIp_UID, strWire("10.10.10.10")) == OK`;
- `"999.1.1.1"` → `OUT_OF_RANGE`;
- `"1.2.3"` → `OUT_OF_RANGE`;
- `"10.10.10."` → `OUT_OF_RANGE`;
- `"a.b.c.d"` → `OUT_OF_RANGE`.

**Подводные камни.**
- `restartDns(ip)` вызывается из `applyConfig()` — при дебаунсе DNS
  пересоздастся один раз на пачку, это нормально.
- Колбэк esp_timer работает в своей задаче: **никакого** wifi/netif API
  из него — только `post`.

**Проверка.** Host-тесты → `idf.py build`.

**СТОП.** Шаг завершён: `checks=N failures=0` + сборка прошла.
Следующий шаг — только по новой команде.

---

## Шаг 8. J1939System: телеметрия без спама и без лока при post

**Цель.** Diff-рассылка runtime-полей TWAI, снятие `AppDataLock` с
пути отправки, читаемые состояния.

**Файлы.**
- `components/03_systems/j1939_system/src/J1939System.cpp`
- `components/01_core/common/include/LogicUtils.h` (проверить
  применимость `updateField`)

**Дефекты.**
1. `updateTwaiStatus()` (`:257-307`): `AppDataLock` удерживается на
   **всём** теле, включая 6 вызовов `sendField` → каждый `post` может
   блокироваться до 50 мс (`EventManager.h:32`) **под мьютексом adata**
   (до 300 мс на цикл телеметрии, блокирует всех писателей AppData).
2. 5-6 полей пушатся каждую секунду безусловно — PUSH-спам, даже если
   ничего не изменилось (хотя `updateField` с diff уже существует).
3. Магические состояния `0/1/2/3` (`:265,278,289,295`) и усечение
   `st.txErr/st.rxErr` в `uint8_t` без проверки (`:273-274` —
   `st.txErr` это `uint32_t`).

**Правки.**
1. Переписать `updateTwaiStatus()`:
   - внутри `AppDataLock` — **только запись** полей
     (`writeFieldScalar` для twaiStarted/twaiState/twaiTxErr/twaiRxErr/
     twaiRecoverCount/activePgns) + сбор того, что реально изменилось;
   - лок закрыть; затем для **изменённых** полей — `sendField(ctx_, uid)`
     (или перевести на `updateField<T>()` из `LogicUtils.h:19`, который
     уже делает diff+push и отправляет **после** лока — предпочтительнее,
     если типы сходятся: uint8/bool/uint32/uint16 — сходятся).
   - особый случай ветки `getStatus()==false` (`:262-270`): записать
     STOPPED-состояние под локом, отправить после.
2. Именованные состояния: в `J1939System.cpp` (анонимный namespace)
   завести
   ```cpp
   enum TwaiStatePub : uint8_t { kStateStopped=0, kStateRunning=1,
                                  kStateBusOff=2, kStateRecovering=3 };
   ```
   и заменить все магические числа (соответствуют документации поля
   `TwaiFields.inc:24`).
3. Clamp ошибок: `static_cast<uint8_t>(st.txErr > 255 ? 255 : st.txErr)`
   (аналогично rxErr), с комментарием про диапазон поля 0..255.

**Подводные камни.**
- `activePgns_UID` пишется под локом отдельно — тоже включить в общий
  паттерн (запись под локом, send после).
- `wsClients_` (atomic) не трогать.
- Проверить, что `updateField` шлёт `COMMUNICATION_SEND`, а не прямой
  broadcast — семантика PUSH не должна измениться (сейчас `sendField` →
  `COMMUNICATION_SEND` → `sendValueFrame(Push)` — идентично).

**Проверка.** `idf.py build`.

**СТОП.** Шаг завершён: сборка прошла. Следующий шаг — только по новой
команде.

---

## Шаг 9. CommModule: дедупликация отправки + таблица NACK-кодов

**Цель.** Убрать дубль malloc+wrap+post и хрупкий `static_cast - 1`.

**Файлы.**
- `components/04_network/communication/src/CommModule.cpp`
- `components/04_network/communication/include/CommModule.h` (если есть
  объявление — glob)

**Дефекты.**
1. `onSnapshot()` (`:158-197`) почти дублирует `sendFrame()`
   (`:201-235`): тот же malloc frame → wrapFrame → malloc ws_message →
   postSized → free. Отличие только в `msgType/flags/sockfd`.
2. `:128`: `static_cast<uint8_t>(st) - 1` — хрупкая связка с
   порядком `FieldWriteStatus` (комментарий `:127` обязан совпадать с
   enum'ом; любая вставка в enum молча ломает протокол).

**Правки.**
1. `onSnapshot()` — заменить тело на вызов `sendFrame()`:
   ```cpp
   sendFrame(J1939Proto::kMsgTypeSnapshot, J1939Proto::kFlagSnapshot,
             snap->data, snap->length, -1);
   ```
   (плюс существующий `ESP_LOGD`). Проверить, что `sendFrame` объявлен
   выше/в заголовке и виден. Поведение идентично: sockfd=-1 → broadcast.
2. Коды NACK — явная таблица вместо `st - 1`:
   ```cpp
   // Коды ошибок PARAM_NACK (PROTOCOL §5): 0=unknown 1=readonly
   // 2=range 3=len. Соответствие FieldWriteStatus — только здесь.
   static uint8_t nackCodeFromStatus(FieldWriteStatus st) {
       switch (st) {
       case FieldWriteStatus::UNKNOWN_UID:      return 0;
       case FieldWriteStatus::READONLY_DENIED:  return 1;
       case FieldWriteStatus::OUT_OF_RANGE:     return 2;
       case FieldWriteStatus::BAD_LENGTH:       return 3;
       default:                                return 0;
       }
   }
   ```
   Заменить использование на `:128`; комментарий `:127` удалить (он
   дублирует таблицу). Проверить, что в `docs/PROTOCOL-J1939.md`
   коды совпадают (сверить, не редактируя протокол).

**Подводный камень.** `onSnapshot` логировал `snap->data[0]` (count) —
сохранить лог в `sendFrame`-пут или до вызова.

**Проверка.** `idf.py build`.

**СТОП.** Шаг завершён: сборка прошла. Следующий шаг — только по новой
команде.

---

## Шаг 10. Server/StaticHandler: утечка static_ctx + ошибки регистрации

**Цель.** Не терять контекст статики при рестарте httpd и не сообщать
`ESP_OK` при нерабочем WS.

**Файлы.**
- `components/04_network/server/src/ServerModule.cpp`
- `components/04_network/server/src/StaticHandler.cpp`
- `components/04_network/server/src/HttpCommon.hpp` (для общей константы)
- `components/04_network/server/src/OtaApi.cpp` (только логирование
  ошибок регистрации)

**Дефекты.**
1. `reg_static_handler()` (`StaticHandler.cpp:247-268`): `new
   static_ctx_t()` (~4 КБ со scratch-буфером) — **не освобождается** в
   `ServerModule::stop()`; при рестарте httpd (NetworkController) каждый
   `begin()` аллоцирует новый → утечка 4 КБ на рестарт.
2. `ServerModule::begin()` (`:61-65`): ошибка регистрации WS только
   логируется, `begin()` возвращает `ESP_OK` — система думает, что
   сервер готов, но `/ws` мёртв. То же `reg_static_handler(server_)`
   (`:67`) — результат игнорируется.
3. `"/littlefs"` продублирован: `ServerModule.cpp:11` и
   `StaticHandler.cpp:249`.

**Правки.**
1. `reg_static_handler()` → возвращать контекст наружу и хранить в
   `ServerModule`:
   - подпись `esp_err_t reg_static_handler(httpd_handle_t server,
     static_ctx_t** out_ctx)` (forward-declare `struct static_ctx_t;`
   в `HttpCommon.hpp` или в заголовке StaticHandler — выбрать одно
     место); в `ServerModule::stop()` после `httpd_stop` —
     `delete staticCtx_; staticCtx_ = nullptr;`.
   - альтернатива (проще): передавать контекст **из** ServerModule:
     `static_ctx_t* ctx = new ...` делает ServerModule, StaticHandler
     только регистрирует. Выбрать вариант с наименьшим числом новых
     деклараций; главное — владение в `ServerModule::stop()`.
2. Ошибки регистрации: `ws_->reg()` != ESP_OK → лог + `goto fail`
   / возврат ошибки `begin()` (модуль `server` в main — проверить
   critical-флаг; если некритичный — вернуть ошибку всё равно, BootManager
   залогирует и продолжит). Результат `reg_static_handler` — аналогично
   вернуть ошибку. Ошибки регистрации URI в `OtaApi::reg` (`OtaApi.cpp:56+`
   сейчас `void`, ошибки `httpd_register_uri_handler` игнорируются):
   изменить `OtaApi::reg` на `esp_err_t` и пробросить в `begin()`.
   Перед правкой прочитать `OtaApi::reg` целиком и учесть порядок
   wildcard-регистрации (комментарий `ServerModule.cpp:53-57`).
3. Константа пути: в `HttpCommon.hpp`
   `constexpr const char* kStaticMountPath = "/littlefs";` — заменить
   оба дубля (`ServerModule.cpp:11`, `StaticHandler.cpp:249`).

**Подводные камни.**
- `httpd_stop` первым, потом `delete` контекста (порядок из `stop()`
  `:73-86` уже правильный — контекст удалять **после** `httpd_stop`).
- Оставшийся путь `ServerModule::begin()`: `if (server_) return ESP_OK;`
  — при повторном begin после успешного первого контекст не пересоздаётся
  (guard есть) — утечка возникала только при stop→begin; учесть.

**Проверка.** `idf.py build`.

**СТОП.** Шаг завершён: сборка прошла. Следующий шаг — только по новой
команде.

---

## Шаг 11. Валидация входящих данных: DNS-парсер и TP-сессии

**Цель.** Закрыть OOB-read в DNS-ответе и некорректную TP.CM-сессию.

**Файлы.**
- `components/04_network/wifi/src/simple_dns_server.cpp`
- `components/03_systems/j1939_system/src/J1939TransportProtocol.cpp`

**Дефекты.**
1. DNS `:221-226`: при `0xC0`-указателе `ptr += 2` **без проверки**
   `ptr + 2 <= end`; затем `:229` `(size_t)(end - ptr)` при `ptr > end`
   заворачивается (отрицательное → огромное size_t) → проверка
   пропускается → `:233-240` `questions_size` больше пакета →
   `memcpy`-OOB-чтение.
2. TP `onTpCm` (`:19-43`): `packets = msg.data[3]` **не валидируется**:
   `packets==0` → `packetsRemaining--` превращается в 255 (`uint8_t`),
   сессия висит до таймаута и «съедает» свободный слот; не сверяется
   согласованность `packets` с `totalLen` (по спецификации
   `packets == ceil(totalLen / 7)`).

**Правки.**
1. DNS — в ветке `0xC0` (перед `ptr += 2`):
   ```cpp
   if ((size_t)(end - ptr) < 2) return 0;
   ptr += 2;
   ```
   и сразу после основного цикла промежуточная защита:
   `if (ptr > end) return 0;` — **до** вычитания `end - ptr` на `:229`
   (переписать условие как `if (ptr > end || (size_t)(end - ptr) < 4)
   return 0;`). Проверить весь цикл, что после каждой арифметики `ptr
   <= end` (метка `ptr += (size_t)c + 1` уже проверяет `ptr > end` —
   оставить).
2. TP — в `onTpCm` после чтения `packets`/`totalLen`:
   ```cpp
   const uint8_t expectedPackets =
       static_cast<uint8_t>((totalLen + 6) / 7);   // ceil(len/7)
   if (packets == 0 || packets != expectedPackets)
       return;   // некорректный BAM — не создаваем сессию
   ```
   (`totalLen` уже проверен на 0/переполнение `:25` — проверку
   `packets` ставить **после** `:25`.)

**Подводные камни.**
- DNS-парсер читает **запрос** и копирует в ответ — обе стороны должны
  остаться в границах `request_len`; после правки повторно пройти по
  коду глазами с учётом, что `questions_size = ptr - question_start`.
- Строгая проверка `packets` может отклонять нестандартные, но
  «работающие» шины — это осознанно (запись в лог при отклонении
  желательна, но в hot-path — `ESP_LOGD`).

**Проверка.** `idf.py build` (host-тестов нет: файлы зависят от IDF).

**СТОП.** Шаг завершён: сборка прошла. Следующий шаг — только по новой
команде.

---

## Шаг 12. Консолидация хардкодов + мелкие чистки

**Цель.** Один источник дефолтов/констант; убрать мёртвые/декоративные
поля; проверка результатов `subscribe()`.

**Файлы.**
- `components/01_core/common/include/HardwareConfig.h`
- `components/01_core/common/include/SystemTiming.h`
- `components/01_core/common/include/fields/TwaiFields.inc`
- `components/01_core/common/include/fields/SnapshotFields.inc`
- `components/03_systems/j1939_system/src/J1939System.cpp`
- `components/02_hardware/twai/include/TwaiDriver.h` (дефолты Config)
- `components/04_network/server/src/WsHandler.{hpp,cpp}`
- `AGENTS.md` (только описание `DataFields.inc`)
- `tests/host/main.cpp`

**Дефекты (дубли и мёртвый код).**
1. `nodeAddr=25`, `timeout=100`: `J1939System.cpp:138,144,170,177` vs
   дефолты `TwaiFields.inc:4,16`.
2. Пины/битрейт: `HardwareConfig.h:9-13` vs дефолты
   `TwaiDriver::Config` (`TwaiDriver.h:16-18`) — `GPIO_NUM_5/4, 250000`
   продублированы.
3. Тайминги: `SystemTiming.h:32-34` (`kSnapshotIntervalMs=250`,
   `kSnapshotTtlMs=2000`) vs дефолты `SnapshotFields.inc:5,11`.
4. Лимит клиентов WS `10` в трёх местах: `WsHandler.hpp:15` (массив),
   `WsHandler.cpp:39` и `:279` (магическое 10).
5. `maxTrackedPgns` (`SnapshotFields.inc:16`) — декоративное: диапазон
   16..128, но `kMaxRecords=128` compile-time, значение пола нигде не
   читается.
6. `DataFields.inc` (корневой, `include/DataFields.inc`) — нигде не
   `#include`-ится (AppData.h подключает 4 доменных `.inc` напрямую);
   в AGENTS.md он заявлен как «X-macro DataFields.inc» — расхождение.
7. `subscribe()` вызывается без проверки результата (примеры:
   `J1939System.cpp:81-88`, `CommModule.cpp:22-33`,
   `ConfigStore.cpp:207-210`, `WifiApModule.cpp:109`).

**Правки.**
1. **Дефолты в HardwareConfig/Timing:**
   - `Hw::kDefaultNodeAddr = 25;`, `Hw::kDefaultTxTimeoutMs = 100;` —
     использовать в `TwaiFields.inc` (дефолты `DATA_FIELD`) **и** в
     фолбэках `J1939System.cpp` (`:138,144,170,177` — заменить
     литералы `25`/`100` на константы). `TwaiFields.inc` уже
     транзитивно видит `HardwareConfig.h` через `AppData.h`
     (AppData.h:7) — проверить порядок include, при необходимости
     `#include "HardwareConfig.h"` в начале `.inc`-файла (`.inc`
     включается внутри `AppData.h`, include guard не даст дубля).
   - Пины/битрейт: в `TwaiDriver.h::Config` дефолты →
     `Hw::kCanTxGpio/kCanRxGpio/kCanBitrate` (включить
     `HardwareConfig.h` в `TwaiDriver.h` — слой 02 может знать 01).
     Если это нарушит «глупость» драйвера (02 не должен знать
     HardwareConfig) — альтернатива: оставить дефолты в TwaiDriver, но
     добавить `static_assert` в `J1939System::begin()` (который всё
     равно передаёт `cfg.tx = Hw::kCanTxGpio`), что значения совпадают.
     Выбрать вариант с static_assert (чище по слоям).
   - Тайминги: `SnapshotFields.inc:5,11` дефолты →
     `Timing::kSnapshotIntervalMs` / `Timing::kSnapshotTtlMs`
     (AppData.h должен видеть `SystemTiming.h` — включить перед
     `.inc`, аналогично п.1a). В `J1939System.cpp:215,340` фолбэки
     `Timing::kSnapshot*` уже на месте — оставить.
2. **kMaxClients в одно место**: `WsHandler.hpp` — `static constexpr int
   kMaxClients = 10;` (public/private по доступности в .cpp), заменить
   объявление массива `connected_clients_[10]`, условие `:39` и
   `local_clients[10]` в `send_to_all_clients` (`:279`).
3. **maxTrackedPgns → читать в `sendSnapshot`**: в
   `J1939System::sendSnapshot()` (`:334+`) после `collect()`:
   ```cpp
   uint16_t maxTracked = SnapshotAccumulator::kMaxRecords;
   ctx_->fields.getByName("maxTrackedPgns", maxTracked);
   if (n > maxTracked) n = maxTracked;   // усечение сверх лимита
   ```
   (после сортировки `order[]` усечение = «свежие первыми», семантика
   поля соответствует описанию «потолок»). Host-тест аккумулятора не
   трогаем — логика в J1939System. Если пользователь сочтёт изменение
   поведения нежелательным — альтернатива: удалить поле из
   `SnapshotFields.inc` и `activePgns`-связку; спросить при выполнении.
4. **DataFields.inc / AGENTS.md**: правка документации — в AGENTS.md
   (раздел «Структура») заменить «X-macro `DataFields.inc`» на фактическую
   схему: `AppData.h` подключает 4 доменных `.inc`
   (`fields/WifiFields.inc` и др.), корневой `DataFields.inc` —
   документационный индекс доменов (генератором фронтенда не
   используется — проверить `frontend/` на ссылки; если используются —
   не удалять, только уточнить описание).
5. **Проверка subscribe()**: в `begin()`-ах перечисленных модулей обернуть
   критичные подписки: `if (!ctx_->events.subscribe(...)) return ESP_FAIL;
   ` (минимум — лог `ESP_LOGE` при false). Подписки в ConfigStore
   (`:207-210`) — critical-модуль → возврат ошибки допустим; в CommModule
   — вернуть ошибку (BootManager залогирует). Не раздувать: достаточно
   проверок результата с логом, где возврат ошибки ломает порядок
   загрузки — оставить лог + продолжение, решение комментировать.
6. Host-тест: добавить проверку новых констант в `test_field_registry`
   или отдельный блок: `getByName("canNodeAddr") == Hw::kDefaultNodeAddr`
   и `snapshotIntervalMs == Timing::kSnapshotIntervalMs` (защита от
   расхождения дефолтов навсегда).

**Подводные камни.**
- `.inc`-файлы включаются многократно с разными макросами —
  `#include` в их начале допустим только с include-guard’ом
  (`HardwareConfig.h`, `SystemTiming.h` его имеют: `#pragma once`).
- static_assert вместо runtime-дублей — предпочтительнее везде, где
  тип/константа доступны на этапе компиляции.

**Проверка.** Host-тесты → `idf.py build`.

**СТОП.** Шаг завершён: `checks=N failures=0` + сборка прошла.
План завершён — сообщить пользователю итог сводной таблицей.

---

## Сводная таблица (заполняется по мере выполнения)

| # | Дефект | Файлы | Статус |
|---|--------|-------|--------|
| 1 | UAF/дубликаты bigData + TTL-утечка | SnapshotAccumulator.cpp | ☑ |
| 2 | OOB-read config.json; гонки dirty_/reset | ConfigStore.cpp | ☑ |
| 3 | Type-confusion слотов unsubscribe | EventManager.h | ☑ |
| 4 | Безлимитный malloc control-frame; client_count_ вне лока | WsHandler.cpp/.hpp, AppEvents.h | ☑ |
| 5 | Запись FixedString всегда OUT_OF_RANGE | FieldRegistry.h/.cpp, SystemStatusModule.cpp | ☑ |
| 6 | Утечки error-path begin(); TX-wait игнорируется | TwaiDriver.cpp | ☐ |
| 7 | N рестартов AP на пачку; невалидный IP | WifiApModule, FieldRegistry.cpp | ☐ |
| 8 | PUSH-спам телеметрии; лок при post; маг. состояния | J1939System.cpp | ☐ |
| 9 | Дубль sendFrame; хрупкий NACK `st-1` | CommModule.cpp | ☐ |
| 10 | Утечка static_ctx_t; молчаливые ошибки регистрации | ServerModule, StaticHandler, OtaApi | ☐ |
| 11 | OOB DNS-парсер; TP packets=0 | simple_dns_server.cpp, J1939TransportProtocol.cpp | ☐ |
| 12 | Хардкод-дубли; maxTrackedPgns; subscribe() | HardwareConfig, TwaiFields, J1939System, WsHandler, AGENTS.md | ☐ |
