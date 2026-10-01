# Ревью архитектуры jscan_esp

Дата: 01.10.2026. Области: надёжность, расширяемость, поддерживаемость +
вопрос целесообразности перевода проекта на C++17.

Проверено обходом кода по слоям 01_core, 02_hardware, 03_systems, 04_network,
05_storage, main, tests/host, CMake/sdkconfig. Пути даны относительно корня
проекта.

---

## 1. Общий вердикт

Архитектура крепкая и заметно выше среднего для embedded-проекта:

- слои `01 ← 02 ← 03 ← 04`, `05 — по необходимости`, односторонние зависимости;
- владение полями по доменам (config/runtime), RAII (`RaiiGuards.h`, `AppDataLock`,
  деструкторы драйверов), правило пяти в `SnapshotAccumulator`;
- декларативный реестр полей (X-macro `.inc`) + `static_assert` на коллизии UID
  и на сверку дефолтов драйвера с `HardwareConfig` (`J1939System.cpp:52-58`);
- батч-кодировщик с CRC16, host-тесты на ядре, размеры файлов адекватны
  (максимум — `ConfigStore.cpp` 467 строк), god-классов нет.

Структурных ошибок проектирования не найдено. Найденные дефекты — это
**крайние случаи и «тихие» места**, которые не мешают работе в штатном режиме,
но проявятся при нагрузке/расширении и стоят времени на диагностику.

---

## 2. Критичные находки (надёжность)

### 2.1 Порча данных в батче J1939 при TP > 255 байт

`components/01_core/common/src/J1939Proto.cpp:76` — `BatchRecord::len`
(`uint16_t`, см. `J1939Proto.h:32`, диапазон «1..1785») сериализуется в
**один байт**: `static_cast<uint8_t>(r.len)`. При TP-пакете длиннее 255 поле
длины молча усечётся, а сами байты данных запишутся полностью
(`J1939Proto.cpp:77-79` использует исходный `r.len`) → фронтенд распарсит
батч криво, при этом **CRC сойдётся**.

Защиты (`r.len > 255 → assert/error`) нет нигде. Противоречие уже
зафиксировано в самом протоколе: `docs/PROTOCOL-J1939.md:93` описывает
`uint8 len // 1..1785`.

Затрагивает и `batchPayloadSize` (`J1939Proto.cpp:50-60`) — он считает полный
`uint16`-размер, то есть расхождение «размер по расчёту vs реальная запись».

**Статус:** фронтенда пока нет — окно исправить протокол бесплатно
(варианты: расширить поле до `uint16_t`, либо капнуть `len ≤ 255` в
`SnapshotAccumulator` + assert). Решение отложено.

### 2.2 Rollback OTA фактически обесценен

`main/main.cpp:16` — `esp_ota_mark_app_valid_cancel_rollback()` вызывается в
первой строке `app_main`, **до** инициализации любых модулей. При включённом
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` (sdkconfig.defaults) образ, который
загружается, но не поднимает сеть/J1939, откатится только если упадёт именно
до этой строки. Нужен перенос после успешного `startAll()` + короткий
health-check.

### 2.3 OTA: нет дедлайна, нет контрольной суммы, стирание до приёма

`components/04_network/server/src/OtaService.cpp`:

- `:105-124` — `HTTPD_SOCK_ERR_TIMEOUT → continue` без общего дедлайна:
  залипший клиент вешает задачу httpd навсегда (FS уже размонтирована `:185`,
  `/ws` и статика мертвы), при этом `WdtPause` (`:41`) на время OTA снимает
  idle-подписку WDT;
- `:197` — `esp_partition_erase_range` идёт **до** получения тела; прерванная
  загрузка (`:240-248`) оставляет устройство без фронтенда;
- `:228` — целостность `.bin` не проверяется (только успешность remount),
  в отличие от app-ветки (`esp_ota_end`, `:338`);
- `:162` — `content_len != part->size → 400`: образ меньшего размера (а он
  почти всегда меньше 1 МБ) отклоняется.

### 2.4 EventManager: нет синхронизации и проверки типа payload

`components/01_core/common/include/EventManager.h`:

- `:43-86, 100-199` — пул подписок (`reserveSubscription`/`releaseSubscription`)
  не защищён ни мьютексом, ни атомикой (в классе нет синхронизации вообще);
  сейчас спасает то, что подписка идёт из main-задачи, но API этого не требует;
- `:111-156, :255-266` — тип/размер данных события не проверяются:
  рассинхрон pub/sub по одному `(base,id)` — UB. Ловушка: `post(...,
  J1939_SNAPSHOT_SEND, snap)` со flexible-array структурой (`AppEvents.h:39-42`)
  скомпилируется и отправит только 8 байт заголовка — `static_assert(!is_pointer)`
  (`:258`) это не ловит;
- `:313-319, :332-338` — коды `esp_event_handler_instance_unregister[_with]`
  игнорируются в `shutdown()`/`unsubscribe()`;
- `:27` — жёсткий лимит 64 подписок прямо в заголовке, рантайм-статистики нет;
- `:291-294` — дропы event-очереди при переполнении только в лог, без счётчика
  (потери видны клиенту как дыры в снапшотах/PUSH);
- `AppContext.h:29` — очередь 256 событий × до 8 КБ: при зависшем event-loop
  возможен всплеск аллокаций, затем дропы после 50 мс. Фолбэк есть, но граница
  грубая для 8 КБ-батчей.

### 2.5 Потери кадров TWAI без телеметрии

`components/02_hardware/twai/src/TwaiDriver.cpp`:

- `:33` — результат `xQueueSendFromISR(rxReadyQueue_)` не проверяется: при
  полной ready-очереди слот навсегда теряется из пула;
- `:27` — если свободных слотов нет, кадр отбрасывается без счётчика;
  `twaiRxErr` (`TwaiFields.inc:36`) видит только аппаратные ошибки шины;
- `:178-191` — `end()` может удалить мьютекс под ожидающей задачей
  (гарантия «вызывать после остановки отправителя» лежит на вызывающем);
- `:271-286` — `recover()` без `txMux_`: латентная гонка при любом втором
  вызывающем.

### 2.6 Жизненный цикл задач: деструкторы убивают живые задачи

- `components/03_systems/j1939_system/src/J1939System.cpp:68-72`
- `components/05_storage/config_store/src/ConfigStore.cpp:172-175`

`vTaskDelete()` без согласования — задачу можно убить внутри `twai_.end()` /
`saveToFs()` (посередине записи в flash). Нет graceful shutdown (флаг + join).
Связано: `ConfigStore::reset()` (`ConfigStore.cpp:450-454`) делает `unlink`
параллельно с работающей autosave-задачью.

### 2.7 Прочее по надёжности

- **TP поддерживает только BAM** — `J1939TransportProtocol.cpp:17`: peer-to-peer
  сессии (RTS/CTS) молча отбрасываются, `kMaxSessions = 2`
  (`J1939TransportProtocol.h:20`); чтение CM без проверки `dlc >= 8`
  (`:22-26`) даёт мусорную длину/PGN.
- **Стек задачи J1939 на пределе** — `J1939System.cpp:376-392`: `order[128]` +
  `BatchRecord[128]` ≈ 3 КБ из 6144 (`:16`), комментарий `:14-15` учитывает
  только «2 КБ батча»; сверху `malloc(8192)` на снапшот (`:420`).
- **Отправка из event-loop может стопорить всю шину** — `WsHandler.cpp:309,345`:
  прямая запись в сокет из задачи event-loop; при забитом TCP-окне у одного
  медленного клиента встают конфиг/снапшоты/команды. Таймаута отправки и
  серверного ping/keepalive нет — мёртвые клиенты держат слоты.
- **`taskENTER_CRITICAL` для задачной синхронизации** — `WsHandler.cpp:27,92,
  121,327`; `reg()` (`:358-359`) сбрасывает `client_count_` вообще без лока.
- **Гонки DNS-сервера** — `simple_dns_server.hpp:26` (`sock_` неатомарный,
  возможен double-close), `simple_dns_server.cpp:142` (`client_len` не
  сбрасывается перед `recvfrom`); стоп DNS блокирует вызывающего до ~2.1 с
  (`:78-84`), а вызывается из event-loop (`WifiApModule.cpp:275-294`).
- **Непроверенные коды netif** — `WifiApModule.cpp:208-210`.
- **Captive-редирект по `strcmp(Host, ip)`** — `StaticHandler.cpp:162`: Host с
  портом не совпадёт → риск петли; матчи доменов — `strstr` по подстроке (`:151`).
- **Потеря автосейва при ошибке записи** — `ConfigStore.cpp:402-430`: при ошибке
  `open/write/rename` `dirty_` не возвращается → изменение теряется.
- **Тихое применение конфига по строке** — `WifiApModule.cpp:184-200`,
  `J1939System.cpp:88,172,178,241,271,304,361,369`: `getByName("apSsid")` вместо
  существующих UID; опечатка вернёт `false` и молча применит дефолт (в
  `applyConfig` результат вообще не проверяется).
- **`AppData` без member-инициализаторов** — `AppData.h:28-33`: до
  `initAppDataDefault` (первый вызов — `ConfigStore.cpp:200`) поля — мусор;
  в деградированном режиме (`BootManager.cpp:57-59`) читатели получат
  неопределённые значения.
- **Игнорируемые возвратные коды** — `LogicUtils.h:10,55` (`events.post`),
  `LogicUtils.h:35` (статус `writeFieldScalar`), `AppContext.h:20`
  (`esp_event_loop_delete`), `FieldRegistry.cpp:107,129` (payload длиннее
  `meta->size` принимается), `FieldRegistry.cpp:161` (`fieldAt` без проверки
  границ), `BootManager.h:13-15,21-22` (`new` без проверки, `add()` без
  `esp_err_t`, лимит `kMaxModules=16`).
- **`AppEvents.h:22-23, 58-63`** — `OTA_BEGIN` документирован с payload
  `ota_begin_event_t`, но постится без данных (`OtaService.cpp:77`).

---

## 3. Расширяемость

### Что сделано хорошо

- Добавление **config-поля** — 1 строка в `fields/*.inc`; UID по хэшу,
  коллизии ловятся на компиляции, автосейв/PUSH-on-connect/JSON подхватываются
  автоматически. Образцовая декларативность.
- Слои с односторонними зависимостями, `REQUIRES` объявлены явно (без
  транзитивности).

### Где ожидается copy-paste

| Что | Где | Проблема |
|---|---|---|
| Новый HTTP-эндпоинт | `OtaApi.cpp:73-107`, `ServerModule.cpp:54-61` | 3 почти одинаковых блока `httpd_uri_t`, порядок регистрации (wildcard `/*` последним) неочевиден, `max_uri_handlers = 32` хардкодом; нет таблицы маршрутов |
| Новая команда протокола | `CommModule.cpp:86-171`, константы `J1939Proto.h:15-22` | ручной парсинг в `case`, валидация длины пишется заново; нет таблицы `{MsgType, handler, min_len}` |
| Побочный эффект config-поля | `J1939System.cpp:191-227`, `WifiApModule.cpp:237-246` | ручной uid-switch; забытый `case` = поле сохранится, но не применится (молча) |
| MIME-тип | `StaticHandler.cpp:49-69` | if-цепочка вместо таблицы `{ext, type}` |
| Дискретные значения | `AppTypes.h:45-52`, `TwaiFields.inc:10` | `CFG_ENUM` объявлен, но не используется: `canBitrate` задан диапазоном 125000..1000000 → принимает 125001, дублирование списка разрешённых значений в `J1939System.cpp:24,35,90` + откат `:97-99` |

### Дублирование

- Список доменов продублирован **4 раза**: `AppData.h:29-32,45-48,89-100,159-162`,
  `AppTypes.h:36-42`, `DataFields.inc:24-27`. Новый домен = правка в 5 местах;
  забытый include не поймает `static_assert` — поле просто «не существует».
- `readUnsignedLE/readSignedLE` — `FieldRegistry.cpp:23-31` vs
  `ConfigStore.cpp:23-31` (при заявленном «едином источнике» `FieldRegistry.cpp:11`).
- Паттерн «old → write → new → memcmp» — `CommModule.cpp:136-152` и
  `ConfigStore.cpp:128-150`; упаковка uid LE — 4 места `CommModule.cpp`;
  блок «lock + state=ERROR + ...» ~10 раз в `OtaService.cpp`;
  `unique_ptr<T, decltype(&free)>` в 5 файлах.

### Прочее

- `BootManager.h:12-17` — после `begin()` указатель на модуль теряется
  (хранится только fn-ptr) → управляемый shutdown модуля невозможен, а
  `EventManager::unsubscribe(void*)` требует именно указатель.
- `AppData.h:169-202` — `findField/getFieldPtr/getFieldAs` дублируют
  `FieldRegistry` и **без мьютекса**; сейчас не вызываются — мёртвый код-ловушка.

---

## 4. Поддерживаемость

- **Размеры файлов адекватны**, ответственности разведены
  (J1939System разбит на Decoder/TP/Accumulator, Comm → J1939Channel/FrameTx):
  `ConfigStore.cpp` 467, `J1939System.cpp` 433, `OtaService.cpp` 395,
  `WsHandler.cpp` 391, `WifiApModule.cpp` 348, `TwaiDriver.cpp` 302,
  `simple_dns_server.cpp` 293, `StaticHandler.cpp` 263, `CommModule.cpp` 215.
- **`ESP_EVENT_DEFINE_BASE` в заголовке** — `AppEvents.h:8`, включается ~15 TU.
  В C++ у `const` внутренняя связь; esp_event сравнивает base **указателем**.
  Сейчас работает только благодаря линковочному слиянию одинаковых строк
  (проверено по `build/JScaner.elf`: 2 вхождения `"APP_EVENTS_BASE"` на 11.8 МБ).
  Нарушена IDF-идиома (DEFINE — в один `.cpp`, DECLARE — в заголовок); смена
  флагов компиляции может **тихо** убить доставку всех событий.
- **Хрупкая генерация `AppData`** — `AppData.h:74-102,155-164`: 4 блока
  `#undef/#define DATA_FIELD` с промежуточными `#undef FIELD_DOMAIN`; порядок
  директив неочевиден. `DataFields.inc:6-8` — «интеллисентная» ветка
  объявляет `type member;`, где `type` не определён (код-мусор).
- **`#pragma GCC diagnostic ignored "-Wcast-function-type"`** открыт на весь
  `EventManager.h:10-11,349` — подавляет предупреждения во всех включающих TU.
- **Мёртвый код / неиспользуемое**: `ConfigStore::applyFieldsWithNotify`
  (`ConfigStore.h:39`, 0 вызовов), 5 из 10 статусов `HttpCommon.hpp:15-23`,
  `AppData::findField` и пр.
- **Тесты**: `tests/host` покрывают только `J1939Proto / FieldRegistry /
  SnapshotAccumulator / Timing`. Не покрыты при том, что чисто тестируемы:
  `J1939Decoder.cpp` (35 стр.), `J1939TransportProtocol.cpp` (98 стр.),
  диспатч `CommModule`, JSON-слой `ConfigStore` (`jsonToWireValue` — static в
  .cpp, недоступен без рефактора).
- **Репозиторий**: `sdkconfig` (100 КБ) закоммичен рядом с `sdkconfig.defaults`
  (+ `sdkconfig.old`, `update.zip`) — риск расхождения конфигов. В корневом
  `CMakeLists.txt:16-21` OTA-пакетирование вешается на POST_BUILD без
  файловой зависимости от `scripts/package_ota.py`.
- **`AGENTS.md`** заявляет «смесь C99/C++17» — фактически C99 в проекте нет
  (см. §5), формулировку стоит поправить.

---

## 5. Целесообразность перевода на C++17

### Факты (проверено по содержимому, не по расширениям)

| Показатель | Значение |
|---|---|
| Файлов на C вне vendored-кода | **0** |
| Проектных файлов (components + main) | 63 файла / 6807 строк — **всё C++** |
| C в проекте | только vendored `components/05_storage/esp_littlefs/` (28 файлов, 19 614 строк, апстрим) + `managed_components/cjson` |
| Единственный собственный `extern "C"` | `main/main.cpp:13` — `app_main` (требование IDF) |
| Фактический стандарт таргета | **`-std=gnu++26`** (дефолт IDF 6.1; проект нигде не переопределяет) |
| Стандарт host-тестов | **`C++17`** (`tests/host/CMakeLists.txt`) — разъехался с таргетом |
| Флаги `-std=` в проектных CMakeLists | отсутствуют (кроме tests/host) |
| Исключения / RTTI | `-fno-exceptions`, `-fno-rtti` (sdkconfig) — 0 `throw`/`dynamic_cast` в коде |
| Уже используются C++17+ фичи | `inline constexpr`, CTAD + deduction guide (`RaiiGuards.h:99`), variadic templates + perfect forwarding (`BootManager.h:11-17`), `unique_ptr`, `mutex`, rule-of-five, `std::is_pointer_v` |
| C++20-фичи уже в коде | designated initializers: `AppContext.h:29-33`, `OtaApi.cpp:73-105`, `WsHandler.cpp:373`, `StaticHandler.cpp:245` (под gnu++26 — валидны) |
| **Не используются нигде** | `std::optional`, `if constexpr`, structured bindings, `std::string_view`, `[[nodiscard]]`, `[[maybe_unused]]`, fold expressions |
| Что осталось «в стиле C» | `malloc/free` (8/5, везде под RAII), `memcpy/memset` (26, парсинг протокола), raw-массивы с фикс. лимитом (осознанно, без heap), C-касты (~30, в основном varargs-логи) |

### Вывод

1. **«Перевод всего проекта на C++17» = нулевая работа**: файлов на C вне
   vendored-кода нет. Проект уже полностью C++ (включая заголовки `.h` с
   `class`/шаблонами/`std::`).
2. **Vendored `esp_littlefs` переводить бессмысленно** — это апстрим с
   собственными тестами; стыкуется через свой `extern "C"` guard, обёртка
   `LittleFsService.{hpp,cpp}` — корректный RAII-move-only класс.
3. Единственное содержательное действие по теме — **синхронизация стандартов**:
   таргет собирается как `gnu++26`, тесты жёстко на `C++17`. Рекомендуется
   поднять `tests/host` до 26 (код уже использует designated initializers, то
   есть формально не весь валиден как strict C++17). Альтернатива — глобально
   зафиксировать 17, но тогда предупреждения при `-Wpedantic`.
4. **Читаемость и надёжность выиграют не от «перевода», а от точечной
   модернизации стиля** (оценка — часы, работа по конкретным файлам, не
   миграция): `std::optional` вместо пар `value+bool`, `[[nodiscard]]` на
   обёртках возврата `esp_err_t`, таблицы вместо свитчей (§3), `.h` → `.hpp`
   для C++-заголовков (22 файла) — чтобы язык был однозначным.
5. Постоянное ограничение, которое никуда не денется: **`-fno-exceptions`**
   (и `-fno-rtti`). Любая C++-фича с исключениями недоступна; `std::string` /
   `std::vector` — можно (IDF так и делает), но путь ошибок — коды/`optional`.

**Итог: перевод на C++17 не требуется — он уже состоялся. Смысл имеют
(а) синхронизация стандартов таргета и тестов, (б) точечная модернизация
стиля, (в) исправление находок §2.**

---

## 6. План работ (предложение, решение отложено)

Приоритеты по соотношению «риск ↔ стоимость».

### Фаза 1 — критичная надёжность

1. Исправить усечение `len` в батче (`J1939Proto.cpp:76`):
   **вариант А** — расширить поле до `uint16_t` в протоколе (фронт ещё не
   написан, окно открыто); **вариант Б** — капнуть `len ≤ 255` в
   `SnapshotAccumulator` + assert. *Требуется выбор варианта.*
2. Перенести `esp_ota_mark_app_valid_cancel_rollback()` (`main.cpp:16`) после
   успешного `startAll()` + короткий health-check.
3. OTA (`OtaService.cpp`): общий дедлайн приёма; не стирать storage до
   получения тела (staging); проверять размер/CRC `.bin`.
4. `EventManager`: мьютекс на пул подписок; проверка типа/размера payload
   (или сделать `postSized` единственным путём для не-POD).

### Фаза 2 — расширяемость

5. Таблицы вместо свитчей: команды протокола (`CommModule`), MIME-типы,
   URI-роутинг; включить `CFG_ENUM` для `canBitrate`.
6. Устранить 4-кратное дублирование списка доменов (один источник + X-macro).
7. Счётчики потерь (TWAI-слоты, event-дропы) — в телеметрию.

### Фаза 3 — гигиена

8. `ESP_EVENT_DEFINE_BASE` перенести в один `.cpp`; удалить мёртвый код;
   проверять возвратные коды; graceful shutdown задач (флаг + join вместо
   `vTaskDelete` в деструкторах).
9. Поднять `tests/host` до gnu++26 (или зафиксировать стандарт глобально);
   покрыть тестами `J1939Decoder`, `J1939TransportProtocol`; убрать
   `sdkconfig.old`/`update.zip` из индекса; поправить `AGENTS.md`.

---

## 7. Полный индекс находок (сокращённый)

Надёжность: §2.1 батч len (J1939Proto.cpp:76) · §2.2 rollback (main.cpp:16) ·
§2.3 OTA (OtaService.cpp:105,197,228,162) · §2.4 EventManager
(EventManager.h:43,111,313,291) · §2.5 TWAI (TwaiDriver.cpp:27,33,178,271) ·
§2.6 задачи (J1939System.cpp:68, ConfigStore.cpp:172,450) · §2.7 TP/стек/WS/
DNS/netif/ConfigStore/строковые обращения/AppData/возвратные коды.

Расширяемость: §3 таблица copy-paste (OtaApi.cpp:73, CommModule.cpp:86,
J1939System.cpp:191, StaticHandler.cpp:49, AppTypes.h:45) · дублирование
доменов (AppData.h) · BootManager lifetime · мёртвый AppData-доступ.

Поддерживаемость: §4 event base (AppEvents.h:8) · хрупкие макросы (AppData.h:74)
· pragma (EventManager.h:10) · мёртвый код · тесты (tests/host) · репозиторий
(sdkconfig.old, update.zip) · AGENTS.md.

C++17: §5 — 0 файлов на C, gnu++26 vs C++17 в тестах, -fno-exceptions.