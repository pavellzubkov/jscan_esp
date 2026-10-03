# План правок по ревью ARCHITECTURE_REVIEW (ARCH_FIX2)

Источник: `docs/ARCHITECTURE_REVIEW.md` (01.10.2026, коммит-база `26f0eea`).
Все пути и номера строк — по состоянию на этот коммит; если разошлось, ищи по
фрагменту кода/контексту.

## Как работать с этим планом

- **Один шаг = одна сессия.** Шаг самодостаточен: цель, файлы, конкретные
  правки, подводные камни, проверка. Следующий шаг — только после того, как
  предыдущий собран, закоммичен и запушен, и только по команде пользователя.
- **После каждого шага: сборка -> host-тесты (если менялись) -> коммит -> пуш -> СТОП.**
  Не накапливать изменения между шагами. Сборка упала — чинить в рамках
  текущего шага и собираться заново.
- **Пометка выполненного шага**: заголовок `## Шаг N. ... ✔`, строка в сводной
  таблице — ☑. Пометка только после успешной проверки.
- «СТОП»: вывести результат проверки пользователю и прекратить работу до новой
  команды.

## Команды проверки

Прошивка (ESP-IDF 6.1, `idf.py` в PATH через idf-exe, из корня проекта):

```powershell
idf.py build
```

Host-тесты — нативно в WSL (Ubuntu-22.04; на Windows g++ нет). Из PowerShell:

```powershell
wsl -d Ubuntu-22.04 -- bash -lc "cd /mnt/e/Projects/Embedded/ESP32/J1939_scaner/jscan_esp && rm -rf build/host && cmake -S tests/host -B build/host && cmake --build build/host -j4 && ./build/host/host_tests"
```

Успешный вывод: `checks=N failures=0` (база на 26f0eea — `checks=106`).
`rm -rf build/host` обязателен, если менялся состав файлов/стабы в
`tests/host/CMakeLists.txt`; иначе можно без него.

---

# Сводная таблица шагов

| # | Шаг | Статус |
|---|-----|--------|
| 0 | Коммит ревью + плана (документы) | ☑ |
| 1 | Протокол: `len` батча -> uint16 | ☑ |
| 2 | Rollback OTA: mark после health-check | ☑ |
| 3 | OTA: дедлайн приёма + staging storage | ☑ |
| 4 | EventManager: mutex пула + счётчики дропов | ☑ |
| 5 | EventManager: compile-time типизация pub/sub | ☑ |
| 6 | TWAI: счётчики потерь + дренаж + неблок. TX | ☑ |
| 7 | Стек задачи J1939: 6144 -> 8192 + убрать массивы | ☑ |
| 8 | WS: отправка из event-loop -> sender-задача | ☑ |
| 9 | Сеть: DNS/captive/netif крайние случаи | ☑ |
| 10 | TP до полноценного (RTS/CTS/EOM) + host-тесты | ☑ |
| 11 | Ресурсы и крайние случаи (AppData/длины/UID) | ☑ |
| 12 | Graceful shutdown задач (J1939/ConfigStore) | ☑ |
| 13 | Гигиена: event base, мёртвый код, sdkconfig, тесты | ☑ |
| 14 | Расширяемость: таблицы, CFG_ENUM, домены, readLE | ☑ |

**Не делаем (вне объёма, зафиксировано):** `.h`->`.hpp` ренейминг 22 файлов;
`std::optional`-модернизация стиля; полное устранение 4 `#include`-проходов
AppData (требует кодогенерации — частично закроем в шаге 14); BootManager
хранение указателей модулей для управляемого shutdown (нет потребителя — шаг 12
делает join через stop-флаг).

---

## Шаг 0. Коммит ревью и плана ✔

**Цель.** Положить в репозиторий `docs/ARCHITECTURE_REVIEW.md` (сейчас
untracked) и этот план.

**Файлы.** `docs/ARCHITECTURE_REVIEW.md`, `docs/PLAN/ARCH_FIX2_PLAN.md`.

**Правки.** Нет (только документы).

**Проверка.** `git status` — оба файла staged. Сборка не нужна.

**Коммит.** `DOCS: ревью архитектуры + план ARCH_FIX2` + push.

**СТОП.**

---

## Шаг 1. Протокол: `len` батча -> uint16 LE ✔

**Цель.** Закрыть ревью 2.1: поле длины записи батча пишется одним байтом
(`static_cast<uint8_t>(r.len)`, `J1939Proto.cpp:76`) — TP-пакет >255 байт
(до 1785) молча усечёт длину, данные запишутся полностью, CRC сойдётся,
фронт распарсит криво. Вариант А (uint16) одобрен. Фронтенда нет — окно
открыто, версию kVersion НЕ поднимаем (v1 ещё никем не потреблялся; если
всплывёт полуготовый клиент со старым layout — поднять).

**Файлы.**
- `components/01_core/common/src/J1939Proto.cpp`
- `docs/PROTOCOL-J1939.md`
- `tests/host/main.cpp`

**Детали (что уже выяснено).**
- `batchPayloadSize` (`J1939Proto.cpp:50-60`): строка 55 `size += 1` для len
  -> `+= 2`.
- `serializeBatch` (`:63-84`): строка 76 `out[pos++] = static_cast<uint8_t>(r.len);`
  -> два байта LE (`r.len & 0xFF`, `(r.len >> 8) & 0xFF`), как у `periodMs`
  строкой ниже. `batchPayloadSize` и `serializeBatch` обязаны быть консистентны
  (иначе `serializeBatch` вернёт 0 «не влезло»).
- `BatchRecord::len` уже `uint16_t` (`J1939Proto.h:32`) — структуру не трогаем.
- `docs/PROTOCOL-J1939.md:93`: `uint8 len // длина data (1..1785)` ->
  `uint16 len // LE, длина data (1..1785)`.
- Пример 3.2 (`PROTOCOL-J1939.md:111-119`): пересчитать payload (2 записи:
  `1 + 2*(1+3+2+8+2)` = 32 -> заголовок `[20 00]`) и `len=08` -> `08 00`
  в обеих строках примера.
- `unwrapFrame` сверяет длину payload по `frame[6..7]` (уже uint16) — не трогать.

**Правки.**
1. `batchPayloadSize`: `size += 2` (коммент «len u16 LE»).
2. `serializeBatch`: 2 байта LE.
3. PROTOCOL-J1939.md: 3.1 поле, 3.2 пример.
4. host-тесты: в `test_proto` поправить ожидание `need` (сейчас
   `1 + 3*(1+3+1+8+2)` -> `1 + 3*(1+3+2+8+2)`) и добавить roundtrip-тест
   записи с `len = 300` (буфер >= 400 байт): сериализовать, проверить 2 байта
   длины, размер payload, что `serializeBatch` != 0.

**Подводные камни.** Старый тест на `need` сломается — это и есть проверка.
Не забыть `docs/PROTOCOL-J1939.md` — он источник истины для фронта.

**Проверка.** `idf.py build` + host-тесты (WSL-команда).

**Коммит.** `ARCH_FIX2 шаг 1: батч len -> uint16 LE (PROTOCOL + host-тесты)` + push.

**СТОП.**

---

## Шаг 2. Rollback OTA: mark после health-check ✔

**Цель.** Закрыть ревью 2.2: `esp_ota_mark_app_valid_cancel_rollback()`
вызывается первой строкой `app_main` (`main/main.cpp:16`) — образ, который
загрузился, но не поднял сеть/J1939, всё равно «валиден». Нужен перенос после
успешного `startAll()` + короткий health-check.

**Файлы.**
- `main/main.cpp`
- `components/01_core/common/include/BootManager.h` (возможно, getter уже есть)

**Детали.**
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` есть и в `sdkconfig:684`, и в
  `sdkconfig.defaults:13` — включён, откат реально работает.
- `BootManager::isReady(const char* name)` уже есть (`BootManager.h:57`,
  `BootManager.cpp:94-101`) — health-check можно строить на нём.
- `startAll` возвращает `firstError`: если упал критичный модуль — возвращает
  его ошибку сразу; если некритичный — degraded mode, `overall != ESP_OK`,
  но система работает. Значит «mark делать только при `overall == ESP_OK`»
  слишком строго: некритичный fail (netctrl/j1939) не должен блокировать
  подтверждение образа, иначе откат будет на рабочей прошивке.
- Решение: mark делать, если поднялся критичный модуль (`isReady("config")`
  — единственный critical в main.cpp:28), плюс health-check задержка.

**Правки.**
1. Убрать `esp_ota_mark_app_valid_cancel_rollback()` с строки 16 (и комментарий).
2. После `boot.startAll(&ctx)` и лога degraded:
   ```cpp
   // Подтверждаем OTA-образ только после успешного старта критичных
   // модулей + короткого health-check: если система падает в первые
   // секунды, mark не выполнится и bootloader откатит прошлое.
   if (boot.isReady("config")) {
       vTaskDelay(pdMS_TO_TICKS(5000));   // health-check: живём 5 с
       esp_ota_mark_app_valid_cancel_rollback();
       ESP_LOGI("MAIN", "OTA image validated");
   } else {
       ESP_LOGE("MAIN", "critical module failed — OTA image NOT validated, rollback on next boot");
   }
   ```
3. Health-check = просто прожить 5 с (падает в первые секунды = откат).
   Если захочется строже — можно добавить `boot.isReady("j1939")`, но это
   уже решение на месте (необязательно).

**Подводные камни.**
- `vTaskDelay(5000)` в `app_main` безопасен — это же главный цикл, дальше
  всё равно `for(;;) vTaskDelay(1000)`.
- `esp_ota_mark_app_valid_cancel_rollback()` вызывать только здесь; не
  плодить вторую точку.
- include `esp_ota_ops.h` уже есть (`main.cpp:9`).

**Проверка.** `idf.py build` (runtime-поведение проверяется на железе:
загрузить битый образ — откат; рабочий — mark в логе через 5 с).

**Коммит.** `ARCH_FIX2 шаг 2: rollback OTA — mark_app_valid после health-check` + push.

**СТОП.**

---

## Шаг 3. OTA: дедлайн приёма + staging storage ✔

**Цель.** Закрыть ревью 2.3 (OtaService.cpp): (а) `HTTPD_SOCK_ERR_TIMEOUT ->
continue` без общего дедлайна залипшего клиента; (б) `esp_partition_erase_range`
ДО приёма тела — прерванная загрузка оставляет устройство без фронтенда;
(в) `content_len != part->size -> 400` отклоняет образ меньшего размера;
(г) нет контроля целостности storage-образа.

**Файлы.**
- `components/04_network/server/src/OtaService.cpp`
- `components/04_network/server/src/OtaService.hpp`

**Детали (текущий код, что выяснено).**
- `recvToCallback` (`OtaService.cpp:90-126`): цикл `while (remaining > 0)`,
  при `HTTPD_SOCK_ERR_TIMEOUT` — `continue` без счётчика (строка 108-110).
  httpd ставит `SO_SNDTIMEO/RCVTIMEO = send/recv_wait_timeout` (5 с по
  умолчанию, `esp_http_server.h` `HTTPD_DEFAULT_CONFIG`) — т.е. один таймаут
  = 5 с, `continue` крутится вечно.
- `handleStorageUpload` (`:131-257`): порядок сейчас — валидация размера
  (`:162`) -> unmount (`:185`) -> **erase (`:197`)** -> recv в erase-раздел
  (`:223`) -> mount (`:228`). На `:223` ошибка приёма = раздел уже стёрт.
- `content_len != (int)partSize` (`:162`) — reject любого меньшего образа.
- `kBufSize = 8192` (`OtaService.hpp:56`), буфер recv уже на heap (unique_ptr).
- App-ветка (`handleAppUpload :262-373`) уже корректна: `esp_ota_begin` пишет
  в НЕактивный OTA-слот, `esp_ota_abort` при обрыве (`:326`) — стирание до
  приёма здесь безопасно (слот не используется системой). Дедлайн нужен и ей —
  он будет общий в `recvToCallback`.
- `WdtPause` (`:38-54`) расширяет Task WDT до 120 с на всё время OTA.
  См. подводный камень про PSRAM.

**Правки (storage-ветка).**
1. Новый дедлайн в `recvToCallback`: параметр `uint32_t timeoutMs` (общий на
   весь приём) +/или счётчик подряд идущих `HTTPD_SOCK_ERR_TIMEOUT`
   (напр. > 10 подряд = 50 с молчания -> `ESP_ERR_TIMEOUT`).
   Реализация: `int64_t startUs = esp_timer_get_time();` в начале, в ветке
   TIMEOUT сравнивать прошедшее время; при превышении — `return ESP_FAIL`
   (и освобождать буфер — он уже за unique_ptr). `esp_timer` доступен,
   в server component надо добавить `esp_timer` в REQUIRES (проверить
   `components/04_network/server/CMakeLists.txt` — если его нет).
2. **Staging в PSRAM**: принимать тело в heap-буфер ДО каких-либо flash-операций:
   - размер: `content_len > 0 && content_len <= part->size` (вместо `!=`);
   - выделить `malloc(content_len)` (устройство N16R8 — 8 МБ PSRAM, образ
     storage-раздела ~1 МБ; если буфер не влез -> 507/500, FS не тронута);
   - `recvToCallback` с existing write-callback, который пишет в этот буфер
     (cb = memcpy со смещением);
   - при ЛЮБОЙ ошибке приёма: освободить буфер, `state_ = ERROR`, ответ 500 —
     **FS вообще не размонтируется и не стирается** (главный выигрыш: прерванная
     загрузка больше не убивает фронт);
   - успех: WdtPause + enterOta + unmount + erase + write из буфера + mount
     (без autoformat, как сейчас) + free буфера.
3. Контроль целостности после записи: read-back verify — прочитать
   `content_len` байт из раздела и `memcmp` с буфером (дешёвая и честная
   проверка для образа без встроенного CRC; littlefs-образ свой CRC не несёт).
   Несовпадение -> state ERROR + mount без autoformat уже есть как вторая
   линия.
4. WdtPause/enterOta оставить только на фазе flash-операций (unmount->mount),
   не на приёме: приём в RAM не блокирует кэш, WDT можно не расширять заранее.

**Правки (app-ветка).** Только общий дедлайн `recvToCallback` (параметр из
п.1). Остальное (`esp_ota_end` = SHA/MD5 заголовка образа, `:338`) уже есть.

**Подводные камни.**
- PSRAM: `malloc` по умолчанию на ESP32-S3 с `CONFIG_SPIRAM=y` идёт в PSRAM
  для крупных блоков (проверить в sdkconfig `CONFIG_SPIRAM_USE_MALLOC=y`);
  если нет — использовать `heap_caps_malloc(size, MALLOC_CAP_SPIRAM |
  MALLOC_CAP_8BIT)`. Проверить sdkconfig перед правкой.
- Не сломать state-машину: `STORAGE_DONE` обязателен перед app-веткой
  (`:267`).
- Ответы клиенту и `error_` тексты оставить/уточнить, фронт пока пуст — можно
  менять свободно.
- Не забыть `error_` под `mux_` (паттерн уже есть).

**Проверка.** `idf.py build` (+ ручной сценарий на железе: оборвать приём —
фронт жив; образ меньше раздела — принимается).

**Коммит.** `ARCH_FIX2 шаг 3: OTA — дедлайн приёма, staging storage в RAM, read-back verify` + push.

**СТОП.**

---

## Шаг 4. EventManager: mutex пула подписок + счётчики дропов ✔

**Цель.** Закрыть ревью 2.4 (часть 1): пул подписок без синхронизации, дропы
event-очереди только в лог, коды unregister игнорируются, нет статистики.

**Файлы.**
- `components/01_core/common/include/EventManager.h`

**Детали.**
- `reserveSubscription`/`releaseSubscription` (`:60-86`), `unsubscribe`
  (`:308-324`), `shutdown` (`:328-344`) — без локов вообще. Сейчас спасает
  то, что всё из main-задачи, но API этого не требует. **Мьютекс:**
  `std::mutex` (методы не из ISR; `post` в мьютекс НЕ засовывать — он уже
  ограничен `kPostTimeoutTicks=50 мс` и логированием).
  - Захватывать в `reserveSubscription`/`releaseSubscription`/`unsubscribe`/
    `shutdown`; НЕ захватывать внутри `esp_event_handler_*_register` (вызов
    под локом удлиняет окно, но unregister/register не рентерабельны снаружи —
    на практике register вызывается уже после резерва, лок можно снять ДО
    register, тогда при ошибке register — releaseSubscription снова под локом).
    Проще: лок на весь метод subscribe целиком — deadlock невозможен, т.к.
    колбэки-подписчики не вызывают subscribe рекурсивно из одного потока,
    который держит лок (подписки идут из main). Оставить простой вариант:
    `std::lock_guard` в начале каждого метода, трогающего пул.
- Дропы: `post/postSized` при `ESP_ERR_TIMEOUT` (`:241,267,291`) — вести
  `std::atomic<uint32_t> droppedEvents_` и `postedEvents_`; геттеры
  `droppedEvents()` / `postedEvents()`; лог оставить (уже есть), но добавить
  счётчик в лог: `"Event queue full, dropped event %d (total dropped=%u)"`.
- Коды unregister (`:315-319`, `:334-338`): `esp_err_t err =
  esp_event_handler_instance_unregister[_with](...)`; при `err != ESP_OK` —
  `ESP_LOGW` (не фейлить shutdown, но видеть).
- Статистика подписок: `subscriptionCount_` уже есть (`:57`); добавить
  геттер `size_t subscriptionCount() const` + лог при исчерпании пула уже
  есть (`"Subscription pool exhausted"` `:102`) — улучшить до
  `"... pool exhausted (%u/%u)"` с `kMaxSubscriptions`.
- `kMaxSubscriptions = 64` (`:27`) оставить как есть (в заголовке — ок,
  рантайм-статистики хватит геттера).

**Подводные камни.**
- `shutdown()` вызывается из `~EventManager` -> `~AppContext`; лок в dtor
  безопасен.
- `unsubscribe(void*)` может вызываться и во время работы — лок обязателен.
- std::mutex в шапке EventManager.h: нужен `#include <mutex>`; EventManager.h
  уже C++ (использует templates) — включение <mutex> в ~15 TU приемлемо.

**Проверка.** `idf.py build` + host-тесты на всякий (EventManager в тесты не
входит — сборка прошивки главная).

**Коммит.** `ARCH_FIX2 шаг 4: EventManager — mutex пула подписок, счётчики дропов, логи unregister` + push.

**СТОП.**

---

## Шаг 5. EventManager: compile-time типизация pub/sub ✔

**Цель.** Закрыть ревью 2.4 (часть 2): «рассинхрон pub/sub по одному
`(base,id)` — UB». **Проверено повторно: компиляционной проверки типов НЕТ.**
`subscribe` (`EventManager.h:94-199`) и `post` (`:255-278`) — независимые
шаблоны, `DataType` дедуцируется с каждой стороны отдельно; единственный
`static_assert` — `!is_pointer` (`:258`). Рассинхрон компилируется и даёт UB
(обработчик получит указатель на чужой тип). Фикс: traits `AppEventPayload<Id>`
+ typed-обёртки с `static_assert` — рассинхрон падает на компиляции.

**Файлы.**
- `components/01_core/common/include/AppEvents.h` (или новый
  `AppEventsTyped.h` рядом — decide на месте, проще в AppEvents.h после
  перечисления событий)
- все call sites поста/подписки (список ниже)

**Дизайн (черновик).**
```cpp
// Тип payload события. Primary template не определён намеренно:
// для не описанного события post/subscribe не соберутся вовсе.
template <app_event_id_t Id> struct AppEventPayload;   // undefined primary

template <> struct AppEventPayload<app_event_id_t::J1939_SNAPSHOT_SEND> {
    using type = j1939_snapshot_t;  static constexpr bool sized = true;
};
template <> struct AppEventPayload<app_event_id_t::J1939_REQUEST> {
    using type = j1939_request_t;   static constexpr bool sized = false;
};
// ... WS_MESSAGE_{RECEIVED,SEND} -> ws_message_t (sized=true),
// WS_CLIENT_{CONNECTED,DISCONNECTED} -> ws_message_t (sized=false, только sockfd),
// WIFI_STATUS, CONFIG_CHANGED, COMMUNICATION_SEND -> соответствующие типы,
// WIFI_REAPPLY / FACTORY_RESET / OTA_BEGIN / OTA_END -> void (без данных).

// Typed-обёртки (делегируют в существующие методы EventManager):
template <app_event_id_t Id, typename T>
bool postEvent(EventManager& em, const T& data) {
    static_assert(std::is_same_v<T, typename AppEventPayload<Id>::type>,
                  "payload type mismatch for event");
    return em.post(APP_EVENTS_BASE, Id, data);
}
template <app_event_id_t Id, typename T>
bool postSizedEvent(EventManager& em, const T* p, size_t n) {
    static_assert(std::is_same_v<std::remove_cv_t<T>,
                  typename AppEventPayload<Id>::type>, "...");
    return em.postSized(APP_EVENTS_BASE, Id, p, n);
}
template <app_event_id_t Id, typename Obj, typename Ret>
bool subscribeEvent(EventManager& em, Obj* obj,
                    Ret (Obj::*m)(const typename AppEventPayload<Id>::type*)) {
    return em.subscribe(APP_EVENTS_BASE, Id, m, obj);
}
// аналогично overload без данных (void payload) и free-fn вариант
```
Смысл: тип в `subscribeEvent` берётся из `Id` (non-deduced context) —
несовпадение метода с trait = substitution failure = ошибка компиляции;
`postEvent` сверяет `T` через `static_assert`. Обработка void-payload
(без данных) — отдельная перегрузка `postEvent<Id>(em)` со
`static_assert(std::is_void_v<...>)`.

**Call sites для миграции (посты, найдено grep'ом).**
- `J1939Channel.cpp:91` (J1939_REQUEST, j1939_request_t)
- `FrameTx.cpp:39` (WS_MESSAGE_SEND, postSized)
- `CommModule.cpp:165` (FACTORY_RESET, без данных)
- `LogicUtils.h:10,55` (COMMUNICATION_SEND, CONFIG_CHANGED)
- `J1939System.cpp:428` (J1939_SNAPSHOT_SEND, postSized)
- `WifiApModule.cpp:54` (WIFI_STATUS), `:270` (WIFI_REAPPLY, без данных)
- `OtaService.cpp:77,82` (OTA_BEGIN/OTA_END, без данных — см. замечание ниже)
- `WsHandler.cpp:57,83,85,113,139` (WS_CLIENT_*, ws_message_t по значению),
  `:287` (WS_MESSAGE_RECEIVED, postSized)

**Call sites подписок.** `ConfigStore.cpp:215,217`; `WifiApModule.cpp:148,154`;
`J1939Channel.cpp:28,30,32`; `CommModule.cpp:44,46,48,50,52`;
`WsHandler.cpp:365`; `J1939System.cpp:113,115`.

**Замечание по OTA_BEGIN.** Ревью 2.7: документирован payload
`ota_begin_event_t`, но постится без данных. Решение (простое): объявить
`AppEventPayload<OTA_BEGIN> = void`, поправить коммент в `AppEvents.h:22,58-63`
(«payload не передаётся; тип оставлен для будущих подписчиков» — или удалить
`ota_begin_event_t`, если он нигде больше не используется — проверить grep;
подписчиков сейчас нет). Сделать выбор на месте: если тип не используется —
удалить, коммент поправить.

**Подводные камни.**
- `EventManager::subscribe` для free-fn/`subscribeDefault` (системные события
  WIFI_EVENT/IP_EVENT) типизацию не трогает — их payload не наши
  (`subscribeDefault :203-229` остаётся как есть).
- Шаблоны в заголовке — убедиться, что `AppEvents.h` включает `<type_traits>`
  (EventManager.h уже включает `:7`).
- Миграция ~16 постов + ~16 подписок: делать в одном шаге (иначе два мира).
  Если разрастётся — можно разбить на 5а (typed-обёртки + traits) и 5б
  (миграция call sites), но коммит один.
- Проверить сборкой, что поймали: намеренно не надо — достаточно того, что
  всё собирается после миграции; уронить тип в любой обёртке -> static_assert.

**Проверка.** `idf.py build` (главное: всё собирается после миграции) +
host-тесты не нужны (не входят).

**Коммит.** `ARCH_FIX2 шаг 5: typed события — compile-time проверка pub/sub (AppEventPayload + postEvent/subscribeEvent)` + push.

**СТОП.**

---

## Шаг 6. TWAI: счётчики потерь + дренаж + неблокирующий TX ✔

**Цель.** Закрыть ревью 2.5 и ответить на вопрос «почему очередь переполняется
при одном владельце». Диагноз (проверено по коду):

1. Владелец один (задача J1939), но она **блокируется** в трёх местах, пока
   ISR сыплет кадры:
   - `sendRequest` -> `twai_.transmit(..., pdMS_TO_TICKS(txTimeoutMs))`
     (`J1939System.cpp:180`): ожидание завершения TX до 100-500 мс
     (`canTxTimeoutMs`, дефолт 100 мс);
   - `sendSnapshot` (`J1939System.cpp:355-433`): сортировка 128 записей +
     сериализация до 8 КБ + `postSized` с таймаутом до 50 мс;
   - `taskLoop` берёт **один кадр за итерацию** (`:260-265`).
2. Пул слотов всего **8** (`TwaiDriver.h:21 rxSlots=8`), а полная шинная
   нагрузка на 250 кбит/с даёт сотни кадров/с — 8 слотов исчерпываются за
   миллисекунды любого простоя.
3. Тогда в ISR `xQueueReceiveFromISR(rxFreeQueue_)` пуст
   (`TwaiDriver.cpp:27`) -> кадр **молча теряется** (счётчика нет).
4. `xQueueSendFromISR(rxReadyQueue_)` (`:33`) физически переполниться не может
   (консервация: free+ready+потребитель = rxSlots = глубина ready), но
   результат не проверен — при будущих правках это ловушка; проверять дёшево.
5. Побочно: `recover()` без `txMux_` (`:271-286`) — латентная гонка.

**Файлы.**
- `components/02_hardware/twai/src/TwaiDriver.cpp`
- `components/02_hardware/twai/include/TwaiDriver.h`
- `components/03_systems/j1939_system/src/J1939System.cpp`
- `components/01_core/common/include/fields/TwaiFields.inc`

**Правки.**
1. **Счётчики в драйвере**: `std::atomic<uint32_t> rxDrops_` (нет свободного
   слота, `:27`) и `txDrops_` (пул TX исчерпан, `:230`); + геттеры
   `rxDrops()/txDrops()`. ISR-атомик на ESP32 (32 бит) — lock-free, ок.
2. **Проверка `xQueueSendFromISR` (`:33`)**: при `pdFALSE` — вернуть слот
   обратно в `rxFreeQueue_` (слот не теряется навсегда) + дроп-счётчик.
   (Сейчас при неудаче слот висит между очередями.)
3. **rxSlots 8 -> 16** (`TwaiDriver::Config::rxSlots`, `TwaiDriver.h:21`).
   Память: слот = `twai_frame_t` + 8 байт ≈ 32-40 байт, 16 слотов ≈ 0.5 КБ —
   бесплатно. Дефолт в Config, статика не нужна.
4. **Дренаж в taskLoop** (`J1939System.cpp:259-265`): пока
   `xQueueReceive(rxReadyQueue, &frame, 0) == pdTRUE` — обрабатывать и
   возвращать слот; т.е. сначала выбирать ВСЕ накопившиеся кадры (timeout 0),
   и только потом таймеры/снапшот. Сейчас один кадр за итерацию + блокирующее
   ожидание `pdMS_TO_TICKS(waitMs)`.
5. **Неблокирующий TX RQST**: `sendRequest` -> `twai_.transmit(id, buf, 3, 0)`
   (timeout=0: «кадр поставлен в очередь драйвера» = успех, ESP_OK;
   `TwaiDriver.cpp:256-257` — блок не ждёт, освободит следующий
   transmit/end). `fail_retry_cnt = -1` (`:139`) — драйвер ретраит сам.
   Поле `canTxTimeoutMs` перестаёт использоваться — НЕ удалять поле (оно в
   конфиге/протоколе), но убрать из `sendRequest` или переиспользовать как
   лимит... Решение на месте: оставить чтение поля, но вызывать transmit с
   `pdMS_TO_TICKS(0)` и логировать `ESP_ERR_TIMEOUT` (очередь драйвера полна —
   редкий случай). Проще: timeout=0 + лог. Поле пометить в комментарии как
   «ограничение драйвера, TX больше не ждёт».
6. **Новый runtime-поле `twaiRxDrops`** в `TwaiFields.inc` (uint32, readonly,
   CFG_UINT, min 0 max 0xFFFFFFFF) — публикация из `updateTwaiStatus`
   (`J1939System.cpp:282-328`) через `updateField<uint32_t>(...,
   twaiRxDrops_UID, twai_.rxDrops())`. UID = fnv1a32(имя) — автоматом;
   `static_assert` коллизий в AppData.h поймает проблемы. Счётчик `txDrops`
   — опционально, можно тоже поле (`twaiTxDrops`) — да, добавить оба.
7. **`recover()` под `txMux_`** (`TwaiDriver.cpp:271`): `LockGuard
   lock(txMux_);` в начале (уже есть `RaiiGuards.h` и `#include` в .cpp).
8. **`end()`** (`:173-192`): review говорит «может удалить мьютекс под
   ожидающей задачей» — реально проблема в порядке вызова
   (`~J1939System` убивает задачу до `twai_.end()` — это шаг 12). В этом
   шаге только комментарий-предупреждение в `end()`: «вызывать после полной
   остановки задач-отправителей (см. ~J1939System, шаг 12)».

**Подводные камни.**
- `rxFreeQueue`/`rxReadyQueue` глубина = `rxSlots` — при смене 8->16 очереди
  создаются в `begin()` из `cfg_.rxSlots` автоматически (`:96-97`), править
  нигде больше не надо.
- Счётчики в ISR: только `atomic` с relaxed/seq_cst, никаких malloc/log в ISR.
- `updateField` для новых полей — следовать паттерну соседей в
  `updateTwaiStatus` (diff внутри updateField, PUSH после снятия лока — уже
  обёрнуто).
- Проверить `REQUIRES` — TwaiFields.inc в `common`, поле добавляется в .inc,
  ничего в CMake не меняется.

**Проверка.** `idf.py build` + host-тесты не обязательны (J1939System не в
тестах), но прогнать для порядка.

**Коммит.** `ARCH_FIX2 шаг 6: TWAI — дроп-счётчики (rxSlots 16, twaiRxDrops/TwaiTxDrops), дренаж в taskLoop, неблокирующий TX RQST, recover под мьютексом` + push.

**СТОП.**

---

## Шаг 7. Стек задачи J1939: 6144 -> 8192 + убрать массивы со стека ✔

**Цель.** Закрыть ревью 2.7 («стек на пределе»): `order[128]` +
`BatchRecord[128]` ≈ 3 КБ из 6144 в `sendSnapshot`, `J1939AssembledMsg`
(1785 байт) в `processFrame`, комментарий `J1939System.cpp:14-15` учитывает
только «2 КБ батча».

**Файлы.**
- `components/03_systems/j1939_system/src/J1939System.cpp`
- `components/03_systems/j1939_system/include/J1939System.h`

**Детали (раскладка стека, что выяснено).**
- `kTaskStackSize = 6144` (`J1939System.cpp:16`).
- `sendSnapshot` (`:355-433`): `size_t order[SnapshotAccumulator::kMaxRecords]`
  (`:376`, 128*4=512 Б на 32-бит) + `J1939Proto::BatchRecord batch[128]`
  (`:392`, sizeof(BatchRecord) ≈ 16-20 Б * 128 ≈ 2-2.5 КБ) — **≈3 КБ локально**.
- `processFrame` (`:330-348`): `J1939AssembledMsg out = {}` (`:340`) — ещё
  ~1.8 КБ на стеке (1785 + заголовок), живёт во время вызова `onTpDt`.
- `malloc(8192)` для снапшота (`:420`) — на куче, не проблема.
- Итого пик ≈ стек задачи + 3 КБ (sendSnapshot) + 1.8 КБ (processFrame, но не
  одновременно с sendSnapshot — разные ветки) + фреймы вызовов/логи.
  6144 с запасом <1 КБ — предсказуемо тесно.

**Правки.**
1. `order[]` и `batch[]` в `sendSnapshot` -> члены класса `J1939System`
   (`std::array<size_t, 128> snapOrder_; std::array<J1939Proto::BatchRecord,
   128> snapBatch_;` в .h, либо static — НЕТ: члены, т.к. задача одна, но
   члены чище и переносимее). Выигрыш ≈3 КБ стека. Сортировка/сборка и так
   только из taskLoop — гонок нет (задача одна), задокументировать.
2. `J1939AssembledMsg out` в `processFrame` — можно вынести в член
   `assembled_` (1.8 КБ -> член, выигрыш ещё 1.8 КБ стека) — та же
   аргументация: одна задача. Сделать.
3. `kTaskStackSize` 6144 -> 8192 (`J1939System.cpp:16`) + переписать коммент
   `:14-15` с реальным раскладом (какие объекты на стеке теперь: только
   локальные мелочи + фреймы; массивы — члены).
4. **Страховочная сетка**: лог HighWaterMark раз в N итераций — в
   `updateTwaiStatus` (его и так зовут раз в с): `uxTaskGetStackHighWaterMark(task_)`
   — минимально свободный стек с момента старта; логировать
   `ESP_LOGI(TAG, "j1939 stack HWM: %u bytes free", ...)` при
   `min < 1024` (или каждый раз в ESP_LOGD).

**Подводные камни.**
- Члены-массивы: sizeof класса вырастет на ~3 КБ — объект `J1939System`
  создаётся через `new` в `makeModule` (heap) — ок.
- `SnapshotAccumulator::kMaxRecords` доступен в .h (`SnapshotAccumulator.h:24`)
  — `std::array<..., SnapshotAccumulator::kMaxRecords>`; в .h уже включён
  `SnapshotAccumulator.h` (`J1939System.h:5`).
- `BatchRecord` в `J1939Proto.h` — в .h надо `#include "J1939Proto.h"` (проверить,
  возможно тянется через J1939TransportProtocol.h — он включает J1939Proto.h:3).

**Проверка.** `idf.py build`. На железе (если есть доступ): посмотреть HWM в логе.

**Коммит.** `ARCH_FIX2 шаг 7: стек задачи J1939 6144->8192, order/batch/assembled — члены класса, лог HighWaterMark` + push.

**СТОП.**

---

## Шаг 8. WS: отправка из event-loop -> sender-задача ✔

**Цель.** Закрыть ревью 2.7 («отправка из event-loop может стопорить всю
шину»): `WsHandler.cpp:309,345` — `httpd_ws_send_frame_async` из задачи
event-loop (подписка `WS_MESSAGE_SEND`, `WsHandler.cpp:365`). Проверено по
исходникам IDF: несмотря на «async», `httpd_ws_send_frame_async`
(`httpd_ws.c:419`) вызывает `sess->send_fn` **синхронно** ->
`httpd_default_send` -> `send()` с `SO_SNDTIMEO = send_wait_timeout`
(5 с). Медленный/мёртвый клиент с забитым TCP-окном блокирует event-loop до
5 с на каждый кадр; broadcast на N клиентов — до N*5 с (по 2 send на кадр:
header + payload). За это время очередь событий (256) копит снапшоты/конфиги
-> дропы после 50 мс таймаута поста.

**Файлы.**
- `components/04_network/server/src/WsHandler.hpp`
- `components/04_network/server/src/WsHandler.cpp`
- `components/04_network/server/src/ServerModule.cpp` (keepalive)

**Детали.**
- Путь события: `FrameTx::send` -> `postSized(WS_MESSAGE_SEND)` ->
  event-loop задача -> `onWsMessageSend` (`WsHandler.cpp:294-315`) ->
  `send_to_all_clients` (`:317-353`) / `httpd_ws_send_frame_async` (`:309`).
- `send_to_all_clients` копирует список сокетов под `taskENTER_CRITICAL`
  (`:327-332`), отправку делает вне лока — но ВСЁ ещё в event-loop задаче.
- `httpd_ws_send_data_async` (`httpd_ws.c:624`) — НАСТОЯЩИЙ async через
  `httpd_queue_work` в задачу httpd + callback. Вариант использования
  есть, но задача httpd тоже одна — медленный клиент займёт её (но httpd
  задача не обрабатывает события — ШИНА НЕ ВСТАНЕТ; встанет только приём WS
  от клиентов). Вариант: (а) своя sender-задача в WsHandler + очередь, (б)
  `httpd_ws_send_data_async` с callback'ом. **Выбираем (а)** — полный контроль
  над таймаутами/дропами, (б) грозит блокировкой httpd-задачи (там свои
  recv-обработчики, приём команд встанет).
- `taskENTER_CRITICAL(&ws_mux)` (`:27,92,121,327`) — задачная синхронизация
  через критсекцию: избыточно (критсекция запрещает прерывания на ядре),
  для чужих задач это не лок. `reg()` (`:358-359`) вообще сбрасывает
  `client_count_`/массив без лока.
- TCP keepalive: `HTTPD_DEFAULT_CONFIG` — `keep_alive_enable = false`
  (`esp_http_server.h:78`); выключение держит мёртвых клиентов вечно.

**Правки.**
1. **Sender-задача + очередь** в `WsHandler`:
   - очередь `xQueueCreate(kTxQueueDepth, sizeof(WsTxItem*))` (глубина 8-16;
     item = malloc-копия `{int sockfd; size_t len; uint8_t data[]}` — т.к.
     payload события валиден только внутри колбэка);
   - `onWsMessageSend` (`:294`): скопировать msg в item, `xQueueSend(..., 0)`;
     при полноте очереди — free(item) + счётчик `txDrops_` (atomic) + редкий
     лог (rate-limit: логировать каждый 10-й дроп);
   - задача-отправитель: `xQueueReceive(portMAX_DELAY)` -> по sockfd/-1 ->
     `httpd_ws_send_frame_async` -> при ошибке `remove_client` (как сейчас);
   - жизненный цикл: создать в `reg()` (один раз, флаг `sender_started_`),
     остановить в `unreg()`/dtor: флаг `stop_` + семафор «задача завершилась»
     (паттерн как в `DnsServer::stop`, `simple_dns_server.cpp:60-98` — там
     готовый образец: doneSem + таймаут + fallback `vTaskDelete`).
2. **Мьютекс вместо `taskENTER_CRITICAL`**: `std::mutex clientMux_` в
   WsHandler; `add_client`/`remove_client`/`cleanup_clients`/
   `send_to_all_clients`/`reg` — под `std::lock_guard`. Логирование — ВНЕ
   лока (паттерн уже соблюдается). `reg()` (`:355-359`) — сброс массива под
   тем же локом.
3. **Keepalive**: в `ServerModule::begin` (`ServerModule.cpp:31-38`):
   `config.keep_alive_enable = true;` (+ `keep_alive_idle=10`,
   `keep_alive_interval=5`, `keep_alive_count=3` — дефолты разумны) — мёртвые
   TCP-клиенты отвалятся сами, их сокеты придут в error -> `remove_client`.
4. **Счётчики**: `txSent_`, `txDrops_` (atomic) + лог в `unreg()`.

**Подводные камни.**
- Копия payload: событие `WS_MESSAGE_SEND` копируется event-loop'ом в свою
  очередь, колбэк получает указатель на эту копию — после возврата из
  колбэка память принадлежит event-loop -> item в очереди обязан быть
  СВОЕЙ копией. Мелкие кадры (PARAM_PUSH ~20 Б) копировать дёшево; снапшот
  8 КБ * 8 слотов очереди = 64 КБ максимум на куче — приемлемо; при OOM
  (malloc == nullptr) — дроп со счётчиком.
- `remove_client` из sender-задачи -> пост `WS_CLIENT_DISCONNECTED` — пост
  из любой задачи разрешён (EventManager), ок.
- PONG в `ws_handler` (`:237`) идёт из задачи httpd — не трогаем (это
  ответы на PING, маленькие).
- Порядок остановки: `unreg()` -> сначала stop sender-задачи (она держит
  server_), потом `cleanup_clients()`. `~WsHandler` уже делает
  unsubscribe (`:20`).
- `kMaxWsMessageLen = 8192` — глубина очереди 8 * 8 КБ = 64 КБ; если PSRAM
  не жалко — ок; если надо экономить — глубина 4.

**Проверка.** `idf.py build`. Runtime-сценарий (на железе/эмуляции): подключить
клиента, забить окно (или выключить), убедиться, что event-loop не встаёт
(снапшоты продолжают приходить остальным).

**Коммит.** `ARCH_FIX2 шаг 8: WS — sender-задача вместо event-loop, мьютекс клиентов, TCP keepalive, счётчики дропов` + push.

**СТОП.**

---

## Шаг 9. Сеть: DNS/captive/netif крайние случаи ✔

**Цель.** Закрыть ревью 2.7 (DNS-гонки, captive-редирект, непроверенные netif).

**Файлы.**
- `components/04_network/wifi/src/simple_dns_server.cpp`
- `components/04_network/wifi/src/WifiApModule.cpp`
- `components/04_network/server/src/StaticHandler.cpp`

**Детали (что выяснено).**
- **Гонка `sock_`**: `stop()` (`simple_dns_server.cpp:66-69`) закрывает
  `sock_` и обнуляет; задача `run()` в цикле (`:141-143`) читает `sock_` в
  `recvfrom`, после выхода снова закрывает (`:162-165`). Два потока пишут/закрывают
  один fd -> возможен double-close (закрытый fd мог переназначиться) и
  read-after-close. Фикс: `sock_` -> `std::atomic<int>` + закрывать только
  обменом `exchange(-1)` (кто первый тот и закрыл).
- **`client_len` не сбрасывается** перед `recvfrom` (`:139-143`): объявлен один
  раз до цикла; после первого recvfrom модифицируется recvfrom'ом — на
  практике ок, но по семантике POSIX надо сбрасывать в `sizeof(client_addr)`
  КАЖДЫЙ раз перед вызовом (иначе возможен OOB при неверном значении).
- **Стоп DNS блокирует до ~2.1 с** (`:78-84`: таймаут 2000+100 мс) и вызывается
  из event-loop: `applyConfig` -> `restartDns` (`WifiApModule.cpp:230`),
  `applyConfig` зовётся из `onWifiReapply` (`:275-294`) — event-loop задача.
  Фикс: `restartDns` (стоп+старт) вынести из event-loop — вариант:
  `onWifiReapply` делает только `esp_wifi_stop/start`, а рестарт DNS повесить
  на `esp_timer`-одноразовую задачу? Проще: стоп DNS **без ожидания 2 с** —
  `stop()` вызывает close(sокет) — задача выходит из recvfrom почти сразу
  (EAGAIN/EBADF через <= 1 с из-за SO_RCVTIMEO=1с; можно уменьшить
  `timeout.tv_sec=1` до `tv_usec=200000` в `:116-117` — выход быстрее).
  Итоговые правки: (а) RCVTIMEO 1с -> 200мс; (б) `stop()` принимать меньший
  таймаут ожидания doneSem (2000 -> 1000 мс достаточно при 200мс цикле);
  (в) сброс `client_len`; (г) atomic `sock_`.
- **Captive-редирект** (`StaticHandler.cpp:133-172`):
  - `strcmp(host_buffer, ap_ip)` (`:162`) — Host с портом (`10.10.10.10:80`)
    не совпадёт -> лишний редирект -> петля для браузеров, шлюзующих порт.
    Фикс: отрезать `:port` из `host_buffer` перед сравнением (первый `:` в
    строке, кроме IPv6 не наш случай — IPv4).
  - `strstr(host_buffer, domain)` (`:151`) — substring-матчи дают ложные
    совпадения (`evilapple.com` совпадёт с `apple.com`). Фикс: точное
    сравнение домена ИЛИ суффикс `.<domain>` в конце Host (после отрезания
    порта). Список доменов (`:139-148`) оставить.
- **Непроверенные netif-коды** (`WifiApModule.cpp:208-210`):
  `esp_netif_dhcps_stop` / `esp_netif_set_ip_info` / `esp_netif_dhcps_start`
  — результаты возвращаются, никто не проверяет. Фикс: `esp_err_t` + лог
  `ESP_LOGE` при ошибке (продолжать — это конфиг AP, не фатально, но видеть).

**Правки.** Сгруппированы выше по (а)-(г) и captive/netif. Файлы — три
перечисленных.

**Подводные камни.**
- `simple_dns_server.cpp` в компоненте wifi — включён ли он в сборку (есть
  в src/ — да, `SRC_DIRS` или явный list — проверить `CMakeLists` wifi).
- Не сломать happy-path captive: после фикса Host-with-port должен ПРОХОДИТЬ
  (`10.10.10.10:80` == ap_ip -> без редиректа) — покрыть комментарием.
- `stop()` вызывается и из `begin()`-путей (`WifiApModule.cpp:80,322`) —
  уменьшение таймаута сократит и их.

**Проверка.** `idf.py build`.

**Коммит.** `ARCH_FIX2 шаг 9: DNS — atomic sock_ + client_len + быстрый стоп; captive — Host:port и точные суффиксы; netif — лог кодов` + push.

**СТОП.**

---

## Шаг 10. TP до полноценного (RTS/CTS/EOM) + host-тесты ✔

**Цель.** Закрыть ревью 2.7 («TP поддерживает только BAM; RTS/CTS молча
отбрасываются; чтение CM без dlc>=8; kMaxSessions=2»). Расширить до
полноценного J1939-TP.

**Файлы.**
- `components/03_systems/j1939_system/include/J1939TransportProtocol.h`
- `components/03_systems/j1939_system/src/J1939TransportProtocol.cpp`
- `components/03_systems/j1939_system/src/J1939System.cpp` (вызовы, TX ответов)
- `tests/host/CMakeLists.txt`, `tests/host/stubs/` (новые стабы), `tests/host/main.cpp`

**Детали (текущий код).**
- `onTpCm` (`J1939TransportProtocol.cpp:14-63`): принимает ТОЛЬКО BAM
  (`data[0] == 32 && dst == 0xFF`, `:17`), остальное молча отбрасывается.
  Формат BAM: `data[1..2]` len LE, `data[3]` packets, `data[5..7]` PGN LE.
- Валидация packets есть (`:34-41`, шаг ARCH_FIX 11): `packets == ceil(len/7)`.
- `onTpDt` (`:65-113`): сборка по 7 байт/пакет, таймаут по
  `Timing::kTransportTimeoutMs` = 1000 мс (`SystemTiming.h:36`), sweep
  таймаутов — только при входящем DT (`:71-75`)! Если DT прекратились
  совсем — сессия висит до следующего DT от того же sa (или вечно). Нужен
  отдельный `tick(nowMs)`/вызов sweep из taskLoop.
- **dlc не проверяется вообще**: `onTpCm` читает `data[1..7]` при dlc<8 —
  мусорная длина/PGN; `onTpDt` — `msg.dlc > 1` частично учтён (`:87`), но
  `msg.data[0]` (номер пакета) при dlc=0 — тоже мусор.
- `kMaxSessions = 2` (`J1939TransportProtocol.h:20`), `Session` ~1.85 КБ
  (`data[1785]`), всего в объекте J1939System.
- `J1939Decoder::decode` даёт `dst` из PS для PDU1 (`J1939Decoder.cpp:24-34`) —
  RTS от чужого узла имеет `pgn == 0xEC00` (TP.CM, PDU2!) — ВНИМАНИЕ: TP.CM
  PGN 0xEC00 — PDU2, поэтому `dst` у сообщения = 0xFF ВСЕГДА; настоящий
  адресат — в `data[1..2]`?? Нет. Разобраться в шаге: PGN 60416 (0xEC00) —
  PDU2, но TP.CM шлётся с PS = адресат (SAE: TP.CM — PDU1? **факт: PGN
  0xEC00 — PDU2, PS используется как destination address для RTS/CTS** —
  в CAN ID поле PS = destination). Decoder для PDU2 (`isP2P=false`) отдаёт
  `dst = 0xFF` и не вытаскивает PS! Т.е. для p2p-TP надо читать PS прямо из
  raw id (`(id >> 8) & 0xFF`) — но это ВЫХОДИТ ЗА РАМЬ «decoder даёт dst».
  Проверить: peerToPeer(0xEC00) -> 0xEC00 > 0 && <= 0xEFFF -> true! Значит
  PDU1... PDU1 = PF < 240. PF у 0xEC00 = 0xEC = 236 < 240 -> PDU1! Значит
  pgnRaw маскируется (`pgnRaw &= 0x1FF00`), pgn = 0xEC00, dst = PS — ВСЁ
  КОРРЕКТНО. TP.CM (0xEC00) и TP.DT (0xEB00) — оба PDU1, dst из PS.
  (0xEB00: PF=0xEB=235 <240 — тоже PDU1.) Хорошо — dst работает.
- Кому отвечать: RTS/CTS — peer-to-peer сессии. Сканер принимает ДАННЫЕ
  (роль приёмника). Кейсы:
  1. **Кто-то шлёт RTS НА НАШ адрес** (dst == canNodeAddr) — мы должны
     ответить CTS (сколько пакетов принимаем), затем принимать DT, в конце
     (EOM) — получатель отвечает EOM? В J1939: RTS -> CTS -> DT... ->
     **EOM шлёт ПОЛУЧАТЕЛЬ** (EndOfMessage, TP.CM control byte = 19), либо
     Abort (255). Отправитель по EOM завершает. Значит мы (получатель)
     отправляем EOM после последнего DT.
  2. **Мы шлём запрос на длинный PGN** -> узел отвечает RTS НАМ -> ветка 1.
  3. **Пассивный сниффинг чужих RTS/CTS** (dst != нас): RTS между двумя
     чужими узлами — мы ВИДИМ все кадры на шине, можем собирать DT
     пассивно (как BAM), CTS от второго узла тоже видим. Это ценный режим
     для сканера. Реализация: на RTS (control=16) создать сессию с
     (sa=отправитель, dst=адресат), НЕ отвечать; DT от sa — собирать; по
     EOM/таймауту — закрыть. CTS (17) — просто триггер «идёт приём»
     (можно игнорировать, DT и так собираем). EOM (19) от получателя —
     закрыть сессию. **Но**: если dst == мы — это case 1 (отвечаем CTS).
- Control bytes TP.CM: 16=RTS, 17=CTS, 19=EOM, 32=BAM, 255=Abort.
- Формат RTS (наш приём): `data[0]=16`, `data[1..2]` = message size LE,
  `data[3]` = total packets, `data[4]` = max packets per CTS (0xFF),
  `data[5..7]` = PGN LE.
- Формат CTS (НАША отправка): `data[0]=17`, `data[1]` = packets we can
  receive, `data[2]` = 0xFF (max), `data[3..4]` = 0xFF, `data[5..7]` = PGN.
  Aдрес: ID = (prio<<26) | (0xEC00<<8) | (dst<<8 для PS!) — PS в PDU1 это
  биты 8..15: `id = (6u<<26) | (0xEC00 << 8)` даёт pgnRaw с PS=0; PS
  вставляется как `| (dst << 8)` — смотреть как это делает `sendRequest`
  (`J1939System.cpp:174-175`: `(id & 0xFFFF00FF) | (dst<<8)` — да, так).
  SA = canNodeAddr, приоритет TP.CM = 7 (обычно 7 для CP; у RQST — 6;
  для TP по SAE приоритет 7... проверить: TP messages use priority 7 for
  CTS/Abort/EOM, 7 для RTS? BAM — приоритет 7. Принять 7.)
- EOM (наша отправка, после последнего DT): `data[0]=19`, `data[1..2]` =
  message size, `data[3]` = total packets, `data[4]` = 0xFF, `data[5..7]` =
  PGN.
- Отправка кадров — из J1939System (у неё `twai_`), TP-класс слой 03 может
  знать только про «что ответить»: интерфейс — `onTpCm` возвращает
  структуру-решение (enum Action {None, SendCts, SendEom} + dst + поля), а
  J1939System транслирует в `twai_.transmit`. Так слой TP остаётся чистым
  и тестируемым на host.
- **Таймауты**: sweep вынести в `void tick(uint32_t nowMs)` — звать из
  taskLoop каждый цикл (или раз в с вместе с телеметрией). Текущий sweep
  внутри `onTpDt` оставить/перенести в tick.
- **kMaxSessions 2 -> 3** (+1.85 КБ в объекте — приемлемо; BAM + пассивный
  RTS + наш RTS).

**Правки (структура).**
1. `onTpCm(msg, out TpAction)`:
   - длина: `if (msg.dlc < 8) return;` (и `msg.dlc < 2` для DT — плюс
     `packetN` читать только при `dlc >= 2`);
   - control = `data[0]`: `32` BAM (как сейчас, dst==0xFF); `16` RTS ->
     если `msg.dst == kMyAddr` (передавать наш адрес в `setLocalAddr` или
     параметром tick/init — J1939System читает `canNodeAddr` из полей и
     задаёт): создать сессию (активный приём) + `out = CTS` с
     `packets = min(maxCtsPackets, ceil(len/7))`; если dst != мы ->
     пассивная сессия (без ответа); `19` EOM -> закрыть сессию (sa,pgn);
     `255` Abort -> закрыть; `17` CTS -> если есть сессия-«ожидание CTS»
     (отправка — у нас нет, т.к. мы не передаём через TP) — для приёмника
     CTS не нужен, игнор.
2. `onTpDt`: валидация `dlc >= 2`; сборка; при `packetsRemaining == 0` ->
   если сессия «наша» (dst==мы) -> `out = EOM` (J1939System отправит);
   если пассивная — просто вернуть собранное.
3. `tick(nowMs)` — sweep таймаутов для всех сессий (перенести из `onTpDt`).
4. `kMaxSessions = 3`.
5. `J1939System::processFrame` (`J1939System.cpp:330-348`): обработать
   `TpAction` -> `twai_.transmit(...)` (RQST уже транслирует PGN -> id —
   TP.CM аналогично, helper в J1939System).
6. Передача нашего адреса в TP: в `begin()`/при `onConfigChanged(canNodeAddr)`
   вызвать `tp_.setLocalAddr(nodeAddr)`.

**Host-тесты (новые!).**
- `J1939TransportProtocol.cpp` подключить к `tests/host/CMakeLists.txt`.
  Ему нужны стабы: `freertos/FreeRTOS.h` (`xTaskGetTickCount`,
  `pdMS_TO_TICKS`, `TickType_t` — управляемый глобальный тик в стабе, чтобы
  тестировать таймауты), `esp_log.h` (уже есть), `SystemTiming.h` — уже
  HOST_TEST-совместим (`:4-6`).
- Тесты: (1) BAM happy-path (собрать 1785 -> len/данные); (2) BAM с
  неверным packets -> сессия не создана; (3) dlc<8 RTS -> ignore; (4)
  RTS на наш addr -> Cts action с нужным числом пакетов; (5) RTS на чужой
  addr -> пассивная сессия, DT собираются, EOM action при завершении;
  (6) таймаут: tick() с увеличенным тиком -> сессия закрыта.
- `J1939Decoder` в тесты подключить можно бесплатно (нужен только
  `TwaiDriver::RxFrame` — тянет `driver/gpio.h` (стаб есть),
  `esp_twai_types.h` (стаб), `freertos/...` (стаб) — если тяжело, отложить
  в шаг 13; TP — обязательны здесь).

**Подводные камни.**
- Приоритет/адрес кадров CTS/EOM: свериться с `sendRequest` для PDU1-ид.
- Пассивные сессии от чужих RTS: не отвечать (иначе двойной получатель на
  шине).
- `kMaxSessions` память: +1.85 КБ heap (объект new'ится) — ок.
- Не сломать ARCH_FIX-тесты packets-валидации (шаг 11 прошлого плана).
- `msg.dst == kMyAddr` для BAM-проверки: BAM требует `dst==0xFF` — BAM-ветка
  не зависит от нашего адреса.
- PDU1-маска в decoder: для PDU1 `pgnRaw &= 0x1FF00` — pgn остаётся 0xEC00
  c нулевым PS — корректно, dst вытащен отдельно.

**Проверка.** `idf.py build` + host-тесты (WSL; состав тестов изменился ->
`rm -rf build/host` обязателен; ожидать checks > 106).

**Коммит.** `ARCH_FIX2 шаг 10: TP RTS/CTS/EOM (активный приём + пассивный сниффинг), dlc-проверки, tick(), kMaxSessions=3, host-тесты TP` + push.

**СТОП.**

---

## Шаг 11. Ресурсы и крайние случаи: AppData, длины, UID-доступ ✔

**Цель.** Закрыть ревью 2.7 (AppData без member-инициализаторов, молчаливое
применение конфига по строке, принимаемый payload длиннее `meta->size`,
`fieldAt` без границ, `OTA_BEGIN` payload, возвратные коды) — блок «ресурсов и
крайних случаев», который просил пользователь.

**Файлы.**
- `components/01_core/common/include/AppData.h`
- `components/01_core/common/src/FieldRegistry.cpp`
- `components/01_core/common/include/FieldRegistry.h`
- `components/01_core/common/include/LogicUtils.h`
- `components/01_core/common/include/AppEvents.h` (если OTA_BEGIN не закрыт в шаге 5)
- `components/03_systems/j1939_system/src/J1939System.cpp`
- `components/04_network/wifi/src/WifiApModule.cpp`
- `components/05_storage/config_store/src/ConfigStore.cpp`

**Детали (что выяснено).**
- **AppData без member-init** (`AppData.h:25-33`): макрос генерит
  `type name;` — до `initAppDataDefault` (первый вызов —
  `ConfigStore.cpp:199-205`, а в degraded mode `BootManager.cpp:57-59`
  (критичный config упал) вызова может НЕ БЫТЬ) поля — мусор. Фикс:
  `type name{};` в определении структуры (`AppData.h:26`) — zero-init.
  ВНИМАНИЕ: zero-init != дефолты (напр. `snapshotIntervalMs` станет 0, а не
  250). Поэтому ДОПОЛНИТЕЛЬНО: `initAppDataDefault` вызвать в конструкторе
  `AppContext` (`AppContext.h:19`, `AppContext() : fields(...) { initAppDataDefault(adata); }`)
  — тогда дефолты есть с самого начала, а `{}` страхует от мусора, если
  порядок сломается. Проверить: `initAppDataDefault` — inline в AppData.h,
  доступен в AppContext.h (включает AppData.h:3).
- **`getByName("apSsid")` и т.п. — 21 вызов** (grep):
  `J1939System.cpp:88,172,178,196,203,210,241,271,304,361,369`,
  `WifiApModule.cpp:184-200` (8 шт), `StaticHandler.cpp:102`.
  Опечатка в строке -> `getMetaByName` возвращает nullptr -> false ->
  молча остаётся дефолт. UID-константы уже существуют
  (`canBitrate_UID` и т.д. генерятся в `AppData.h:155-164`).
  Фикс: новый шаблон `getByUid<T>(uint16_t uid, T& out)` в `FieldRegistry.h`
  (аналог `getByName` :78-82, но `getMetaByUid`); мигрировать все 21 вызов
  на `*_UID`. Опечатка в имени UID -> ошибка компиляции (константа не
  определена). В `applyConfig` (`WifiApModule.cpp:192-200`) результаты
  проверять: если false -> `ESP_LOGE` + return error (сейчас `applyConfig`
  вообще не проверяется в `onWifiReapply` :278 — там проверяется return,
  ок). В `J1939System::onConfigChanged` — лог при false (не фейлить).
- **Payload длиннее meta->size принимается**: `FieldRegistry.cpp:129`
  (`payloadLen < meta->size` -> false, но `>` проходит!): `memcpy(dst,
  payload, meta->size)` (`:130`) молча отрезает хвост. В `writeField`
  (`:206-225`) — вызывается валидация `validateField` (`:107`: тоже только `<`).
  Фикс: для не-строковых полей требовать `payloadLen == meta->size`
  (в `validateField` и `deserializeField`); для строк логика со слайсом уже
  сверяет точную длину (`:120`). Исключение: `CFG_FLOAT` — `payloadLen <
  sizeof(float)` (`:100`) -> `!= 4`. Проверить, что wire-формат чисел =
  ровно `meta->size` байт (см. PROTOCOL §2 и `readField`) — да, `meta->size`.
  Внимание: `CommModule::PARAM_SET` передаёт `payload+2, payloadLen-2`
  (`CommModule.cpp:141`) — клиент мог слать и больше; теперь NACK err=3
  (len) — корректное поведение.
- **`fieldAt` без границ** (`FieldRegistry.cpp:161-163`): добавить
  `if (index >= kAppFieldCount) return g_fieldMeta[0];` — не очень красиво.
  Лучше: assert-семантика — в host-тестах `assert`, на прошивке
  `ESP_ERROR_UNREACHABLE`? Проще и безопаснее: `const FieldMeta* fieldAt(size_t)`
  возвращать ptr + nullptr. Но вызывающий (`CommModule.cpp:218-222` цикл по
  `fieldCount()`) — корректен. Минимальный фикс: bounds-проверка + возврат
  первого элемента? Нет. Решение на месте: поменять на указатель
  (`fieldAtPtr`) потребует правок вызовов (1 место: `CommModule.cpp:219`).
  Сделать: `const FieldMeta* fieldAt(size_t) const` -> nullptr при выходе,
  в CommModule `if (!m) continue;`.
- **`dirty_` не возвращается при ошибке записи** (`ConfigStore.cpp:402-430`):
  `dirty_.exchange(false)` в `:380` ДО записи; при `open/write/rename` ошибке
  -> `return` с `dirty_ == false` -> изменение теряется навсегда. Фикс: на
  каждом error-path (open fail :403-407, write fail :414-418, rename fail
  :426-430) — `dirty_.store(true);` перед return (автосейв повторит через
  секунду).
- **`AppEvents OTA_BEGIN`** (если не закрыто в шаге 5): ревью 2.7 —
  документирован payload `ota_begin_event_t`, пост без данных
  (`OtaService.cpp:77`). Проверить grep'ом использование `ota_begin_event_t`;
  если только в AppEvents.h — удалить тип, поправить коммент.
- **Возвратные коды**:
  - `LogicUtils.h:10,55` — `events.post(...)` результат игнорируется.
    `post` и так логирует дроп внутри EventManager -> добавить `(void)` с
    комментарием? Ревью просит проверять. MINOR: `if (!...post(...))
    ESP_LOGW` — но лог дублируется (EventManager уже логирует). Решение:
    оставить как есть + коммент «дроп логируется в EventManager» — закрыть
    пункт документацией. Или `[[nodiscard]]` на `post`? Это сломает ~10
    мест... где и так игнорируют осознанно. НЕ делать `[[nodiscard]]`.
    Коммент в LogicUtils.
  - `AppContext.h:20` — `esp_event_loop_delete` игнор: `if (esp_event_loop_delete(...) != ESP_OK) ESP_LOGW`.
  - `FieldRegistry.cpp:107,129` — закрыто пунктом про длины.
  - `FieldRegistry.cpp:161` — закрыто полем fieldAt.
  - `BootManager.h:13-15` — `new` без проверки: `Module* m = new (std::nothrow) Module(...); if (!m) return ESP_ERR_NO_MEM;` (включить `<new>`).
  - `BootManager.h:21-22` — `REGISTER_MODULE` игнорирует возврат `add()`:
    в макрос `if ((boot).add({...}) != ESP_OK) ESP_LOGE(...)` — макрос
    раскрывается в выражение; обернуть в do-while(0) с проверкой? Макрос
    сейчас — выражение `(boot).add({...})`. Сделать: `(void)(boot).add(...)`
    и положиться на лог внутри `add()` (`BootManager.cpp:9-10` уже логирует
    ERROR при лимите) -> закрыть документацией. kMaxModules=16 > 5 модулей —
    запас есть.

**Правки.** Список выше по пунктам; порядок произвольный (один коммит).

**Подводные камни.**
- `getByUid` — не сломать строковые поля: `getByName` для `FixedString`
  сверяет `m->size == sizeof(T)` — та же логика в `getByUid`.
- Строгое `payloadLen == size` — проверить все config-поля: wire-формат
  чисел в PROTOCOL §2 = `meta->size` байт? Прочитать `PROTOCOL-J1939.md`
  §2 (serialize rules) и `FieldRegistry::readField` — если строки wire = 1+n,
  числа = size. Если есть исключения (например bool = 1 байт = size) — ок.
- zero-init `{}` для `FixedString` — у него конструктор по умолчанию `= ""`,
  `type name{}` вызовет его — ок.

**Проверка.** `idf.py build` + host-тесты (FieldRegistry в тестах! менять
`validateField` -> обновить/добавить тесты в `tests/host/main.cpp`
(`test_registry`): payload длиннее size -> отказ; `payloadLen == size` ->
ок).

**Коммит.** `ARCH_FIX2 шаг 11: AppData zero-init + default в AppContext, getByUid вместо getByName, строгие длины payload, dirty_ при ошибке, fieldAtPtr, возвратные коды` + push.

**СТОП.**

---

## Шаг 12. Graceful shutdown задач (J1939/ConfigStore) ✔

**Цель.** Закрыть ревью 2.6: `vTaskDelete()` без согласования в деструкторах
— задачу можно убить внутри `twai_.end()` / `saveToFs()` (посередине записи в
flash). Связано: `ConfigStore::reset()` делает `unlink` параллельно с
работающей autosave-задачей (`ConfigStore.cpp:450-454`).

**Файлы.**
- `components/03_systems/j1939_system/include/J1939System.h`
- `components/03_systems/j1939_system/src/J1939System.cpp`
- `components/05_storage/config_store/include/ConfigStore.h`
- `components/05_storage/config_store/src/ConfigStore.cpp`

**Детали.**
- `J1939System::~J1939System` (`J1939System.cpp:66-81`): `vTaskDelete(task_)`
  -> потом `twai_.end()`. Убийство задачи внутри `twai_.transmit` (под
  `txMux_`) -> мьютекс навсегда занят -> `end()` виснет. Или убийство внутри
  `processFrame` (слот не возвращён) — в деструкторе это «всё равно удаляем»,
  но `twai_.end()` подождёт мьютекс.
- `ConfigStore::~ConfigStore` (`ConfigStore.cpp:166-176`): `vTaskDelete(task_)`
  — задача может быть внутри `saveToFs()` (запись в flash: open/write/rename).
- Образец готового graceful-паттерна УЖЕ ЕСТЬ в проекте:
  `DnsServer::stop()` (`simple_dns_server.cpp:56-98`): флаг `running_`
  (atomic) -> задача выходит из цикла -> `xSemaphoreGive(doneSem_)` ПЕРЕД
  `vTaskDelete(nullptr)` -> стоппер ждёт семафор с таймаутом -> fallback
  `vTaskDelete(task_)` + лог. Переиспользовать эту схему 1-в-1.
- `ConfigStore::reset()` (`:439-456`): вызывается из FACTORY_RESET-хендлера
  (event-loop задача). Сейчас: сброс дефолтов -> `unlink` -> `esp_restart`.
  Гонка: autosave-задача в это время может писать файл (dirty_ ->
  saveToFs) -> после unlink файл воскреснет? Нет — `esp_restart` через
  миллисекунды, но запись может успеть ПОСЛЕ unlink -> файл есть, дефолты
  не применятся при следующем старте (config.json перезапишет дефолты).
  Фикс: перед unlink — остановить autosave (stop-флаг + join/семафор, как в
  dtor), потом unlink, потом restart. Проще: в `reset()` установить
  `stopFlag`, дождаться `doneSem` (с таймаутом), затем unlink.

**Правки.**
1. `J1939System`: `std::atomic<bool> stop_{false}` + `doneSem_` (бинарный
   семафор, создать в `begin()` рядом с очередью, удалить в dtor после
   ожидания); `taskLoop` -> `while (!stop_)`; в конце `taskLoop` (выход по
   флагу): `xSemaphoreGive(doneSem_); vTaskDelete(nullptr); return;`.
   Деструктор: `stop_ = true;` -> ждать `doneSem_` до ~2000 мс -> при
   таймауте `ESP_LOGE` + fallback `vTaskDelete(task_)` (как DnsServer) ->
   дальше `unsubscribe`, `twai_.end()`, `vQueueDelete(reqQueue_)`.
   ВАЖНО: порядок в dtor сейчас: vTaskDelete -> unsubscribe -> end
   (`:66-81`) — сохранить: сначала остановить задачу (она не должна работать
   с отписанными обработчиками/закрытым TWAI), потом unsubscribe, потом end.
2. `ConfigStore`: аналогично `stopFlag` + `doneSem_`; `autoSaveLoop`
   (`:462-467`): `while (!stop_) { vTaskDelay(1000); if (dirty_) saveToFs(); }`
   + give + self-delete. Деструктор: stop -> ждать -> fallback vTaskDelete.
   ВАЖНО: в `autoSaveLoop` после `vTaskDelay` проверять флаг ДО `saveToFs`
   (не начинать новую запись после stop).
3. `ConfigStore::reset()`: перед `unlink` — остановить autosave (stop +
   join с таймаутом 1-2 с), затем unlink, затем `esp_restart`.

**Подводные камни.**
- `begin()` ConfigStore: задача создаётся в `:204`; если `begin()` упал ДО
  создания задачи — `task_ == nullptr` -> dtor не ждёт (проверка
  `task_ != nullptr` перед ожиданием, как в DnsServer `:61`).
- J1939System: `begin()` создаёт задачу последней (`:131`) — при ошибке до
  неё `task_ == nullptr`.
- doneSem создавать в begin, удалять в dtor ПОСЛЕ ожидания задачи (паттерн
  DnsServer `:94-97`).
- `stop_` в taskLoop: `while (!stop_)` обёрнуть ТОЛЬКО тело цикла;
  `xQueueReceive` с таймаутом `waitMs` (до 250 мс) — проснётся по таймауту
  и выйдет; для мгновенного выхода можно брать `min(waitMs, 100)` — не
  обязательно, 250 мс ожидания в dtor приемлемы (в пределах таймаута 2000 мс).
- `saveToFs` может идти НЕ в autosave-задаче? Проверить вызовы: из
  `onConfigChanged`? Нет — там только `dirty_ = true` (`:230` и др.).
  `saveToFs` вызывается только из `autoSaveLoop`? grep — и из `reset`? Нет.
  Проверить grep `saveToFs` перед правкой.

**Проверка.** `idf.py build` + host-тесты (ConfigStore не в тестах — сборка).
На железе: factory reset несколько раз подряд под нагрузкой конфига.

**Коммит.** `ARCH_FIX2 шаг 12: graceful shutdown задач J1939/ConfigStore (stop-флаг + семафор + fallback), reset() — стоп autosave до unlink` + push.

**СТОП.**

---

## Шаг 13. Гигиена: event base, мёртвый код, sdkconfig, тесты ✔

**Цель.** Закрыть ревью 4 (поддерживаемость) и 5 (синхронизация стандартов):
`ESP_EVENT_DEFINE_BASE` в заголовке; мёртвый код; `pragma` на весь TU;
`AGENTS.md` (C99); `sdkconfig.old`/`update.zip`; host-тесты до C++20 + тесты
Decoder.

**Файлы.**
- `components/01_core/common/include/AppEvents.h` + новый
  `components/01_core/common/src/AppEvents.cpp`
- `components/01_core/common/CMakeLists.txt`
- `components/05_storage/config_store/include/ConfigStore.h` + `src/ConfigStore.cpp`
- `components/01_core/common/include/AppData.h`
- `components/01_core/common/include/EventManager.h`
- `components/04_network/server/include/HttpCommon.hpp` (проверить путь:
  лежит в `server/src/HttpCommon.hpp` по grep — реально в src)
- `tests/host/CMakeLists.txt`, `tests/host/main.cpp`
- `.gitignore`, `AGENTS.md` (не в git — правится локально, коммит не нужен)
- корневой `CMakeLists.txt` (OTA package DEPENDS)

**Детали.**
- **`ESP_EVENT_DEFINE_BASE(APP_EVENTS_BASE)`** в `AppEvents.h:8`: в C++
  `const` объект имеет внутреннюю связь -> в ~15 TU своё определение
  (линкер мог слить одинаковые const в COMDAT — «работает случайно»; esp_event
  сравнивает base **указателем**, `esp_event.c:277 it->base == base`).
  Фикс (IDF-идиома): в заголовок `ESP_EVENT_DECLARE_BASE(APP_EVENTS_BASE);`,
  в новый `src/AppEvents.cpp`: `#include "AppEvents.h"` + 
  `ESP_EVENT_DEFINE_BASE(APP_EVENTS_BASE);`. Добавить `src/AppEvents.cpp`
  в SRCS в `common/CMakeLists.txt` (там явный список `:1-6` — SRC_DIRS нет).
  После правки ОБЯЗАТЕЛЬНО убедиться, что события работают (build ->
  проверить, что один адрес base: `grep -c APP_EVENTS_BASE build/*.map` или
  просто сборка + рантайм-лог на железе; минимально — сборка, т.к. смена
  линковки может тихо сломать доставку — сверить в .map файле 1 определение).
- **Мёртвый код**:
  - `ConfigStore::applyFieldsWithNotify` (`ConfigStore.h:39`,
    `ConfigStore.cpp:363`?) — 0 вызовов (grep подтвердить) -> удалить.
  - `AppData::findField/getFieldPtr/getFieldAs` (`AppData.h:169-202`) —
    grep: используются только внутри AppData.h -> удалить (мёртвый
    код-ловушка без мьютекса).
  - `HttpCommon.hpp` статусы: 5 из 10 не используются — grep `kAccepted`,
    `kNotModified`, `kMethodNotAllowed`, `kServiceUnavailable`, `kNotFound`
    и удалить неиспользуемые (или оставить — они «единая точка правды»;
    ревью просит удалить — удалить действительно неиспользуемые).
- **`#pragma GCC diagnostic ignored "-Wcast-function-type"`** (`EventManager.h:10-11,349`)
  — открыт на весь TU. Сузить: обернуть только тела subscribe-лямбд? Пragma
  push/pop вокруг конкретных reinterpret_cast нельзя (они в шаблонах).
  Альтернатива: подавить локально `#pragma GCC diagnostic push/ignored/pop`
  вокруг ОДНОГО определения typedef/cast-хелпера... В шаблонах касты в
  лямбдах — сузить невозможно. РЕАЛЬНЫЙ фикс: `#pragma` в заголовке действует
  на все включающие TU — перенести касты в не-шаблонную inline-функцию в
  .cpp? Типы шаблонные... Практичный компромисс: ОСТАВИТЬ как есть, но
  добавить комментарий, почему (шаблонные касты). Ревью-пункт закрыть
  комментарием? Слабо. Другой путь: `reinterpret_cast` -> `union`-каст или
  `memcpy`? Для указателя на метод — нет. РЕШЕНИЕ: оставить pragma, усилить
  комментарием «предупреждение подавляется только из-за обобщённого хранения
  указателей на метод; не расширять на другие TU-диагностики». Отметить в
  плане как «закрыто комментарием — лучшее, что можно без переработки пула
  подписок».
- **`sdkconfig.old` / `update.zip`**: `.gitignore` УЖЕ содержит оба
  (`sdkconfig.old`, `update.zip`) — проверить `git ls-files` (выполнено:
  в индексе их НЕТ, только `sdkconfig` и `sdkconfig.defaults`). Т.е. пункт
  ревью частично уже закрыт. Остаётся: `sdkconfig` (100 КБ) закоммичен —
  ревью предлагает убрать риск расхождения с `sdkconfig.defaults`. РЕШЕНИЕ:
  НЕ удалять sdkconfig из индекса (пересборка на чистой машине без sdkconfig
  пересоздаст его из defaults — это нормальная IDF-практика, но локальная
  машинa со своими правками sdkconfig потеряет их). Компромисс: добавить
  комментарий в `sdkconfig.defaults` «sdkconfig — локальная копия, источник
  дефолтов здесь» + убрать `sdkconfig` из индекса (`git rm --cached sdkconfig`
  + в .gitignore раскомментировать `# sdkconfig`). РЕШЕНИЕ НА МЕСТЕ
  (спросить пользователя): убрать sdkconfig из индекса или оставить. В плане
  отметить как «требует решения».
- **`AGENTS.md`**: «Смесь C99/C++17» -> фактически C99 нет (0 файлов), таргет
  `gnu++26`. Поправить строку в `AGENTS.md` (файл в .gitignore — правка
  локальная, коммит не нужен). Также добавить в AGENTS.md строку про
  `ARCH_FIX2`-план? Не обязательно.
- **Корневой CMakeLists.txt:16-21**: OTA-пакетирование на POST_BUILD без
  `DEPENDS` от `scripts/package_ota.py`. Фикс: `add_custom_command(TARGET app
  POST_BUILD ...)` не поддерживает DEPENDS напрямую (это target-level).
  Рабочий путь: обернуть в `add_custom_command(OUTPUT ...)` + `add_custom_target(... ALL)` с `DEPENDS ${CMAKE_CURRENT_LIST_DIR}/scripts/package_ota.py`
  ${build_dir}/JScaner.bin` — или проще: `idf_build_get_property` +
  `set_property(TARGET app APPEND PROPERTY ADDITIONAL_DEPS ...)`? IDF
  использует `esp32_project_add_binary_target`... ПРОСТОЕ РЕШЕНИЕ: оставить
  POST_BUILD, но добавить нулевую проверку — на практике script меняется
  редко. Ревью-пункт MINOR: сделать `COMMAND python ...` + `DEPENDS` нельзя
  для TARGET form — реализовать через промежуточный `add_custom_command(OUTPUT
  ${build_dir}/update.zip COMMAND ... DEPENDS package_ota.py app.bin)` +
  `add_custom_target(package_ota ALL DEPENDS ${build_dir}/update.zip)`.
  Сделать аккуратно, проверить что update.zip появляется.
- **host-тесты -> C++20**: `tests/host/CMakeLists.txt:4` `set(CMAKE_CXX_STANDARD 17)`
  -> 20 (таргет gnu++26, минимум для designated initializers — 20). gcc в
  WSL Ubuntu-22.04 = 11 -> C++20 поддерживает. Если хочется ближе к 26 —
  23 тоже поддерживается gcc11 частично; взять 20 (безопасно).
- **Тесты J1939Decoder** (ревью: «чисто тестируем, 35 стр.»): подключить
  `J1939Decoder.cpp` к host-тестам. Нужные стабы: `esp_twai_types.h`
  (для `TwaiDriver.h`: `TWAI_FRAME_MAX_LEN`, `twai_frame_t`... Тянется
  `TwaiDriver.h` -> `driver/gpio.h` (стаб есть), `esp_attr.h` (IRAM_ATTR —
  нужен стаб!), `esp_twai_types.h` (нужен стаб: `TWAI_FRAME_MAX_LEN`,
  `twai_frame_t`... в хедере TwaiDriver.h используются `twai_frame_t`,
  `twai_node_base*`... это тяжело стабить). АЛЬТЕРНАТИВА: тестировать
  `J1939Decoder::peerToPeer` + decode через выделение лёгкого входа: decode
  принимает `TwaiDriver::RxFrame` (только id/dlc/data — НО тип объявлен в
  TwaiDriver.h, который тянет IDF-хедеры). СТАБЫ: `esp_attr.h` (пустой define
  IRAM_ATTR), `esp_twai_types.h` (typedef struct { uint32_t id; uint32_t
  ide:1; uint32_t rtr:1; uint32_t dlc:4; } ...). Проверить что реально
  нужно TwaiDriver.h: `twai_frame_t` (в TxBlock), `twai_node_base*`,
  `TWAI_FRAME_MAX_LEN`, `gpio_num_t` (стаб есть), `SemaphoreHandle_t`
  (freertos — нужен стаб `freertos/semphr.h`, `freertos/queue.h`,
  `freertos/FreeRTOS.h`, `esp_err.h`...). Это ~5-6 мелких стабов —
  реально сделать: freertos stub (типы Handle_t, xQueueCreate и т.п. как
  no-op/macro), esp_err, esp_attr, esp_twai_types. Работа есть, но она
  одноразовая и закроет будущие тесты слоя 03. ОБЪЁМ: шаг 13 может
  разрастись -> при необходимости разбить на 13а (гигиена) и 13б (тесты
  Decoder + стандарт). КАК РЕШИТЬ НА МЕСТЕ: если стабы даются легко (30
  минут) — в одном; нет — Decoder-тесты перенести в конец плана отдельным
  шагом 15. В плане отметить «оценить трудоёмкость стабов на месте».
  Минимум для шага: C++20 + гигиена.

**Правки.** Список выше. Порядок: event base -> мёртвый код -> CMake OTA ->
тесты стандарт -> sdkconfig (решение) -> AGENTS.md.

**Подводные камни.**
- `ESP_EVENT_DEFINE_BASE` -> один .cpp: проверить, что `AppEvents.h`
  включается в C-контексте? Нет — проект C++. `DECLARE` в заголовке —
  `extern esp_event_base_t const id` (`esp_event_base.h`). После правки
  глянуть map-файл/`nm`: определение одно.
- Удаление мёртвого кода: сначала grep-подтверждение 0 вызовов.
- `git rm --cached sdkconfig` — если решено убрать.

**Проверка.** `idf.py build` + host-тесты (стандарт менялся ->
`rm -rf build/host`).

**Коммит.** `ARCH_FIX2 шаг 13: ESP_EVENT_DEFINE_BASE в .cpp, мёртвый код, host-тесты C++20, OTA package DEPENDS` + push.

**СТОП.**

---

## Шаг 14. Расширяемость: таблицы вместо свитчей, CFG_ENUM, домены, readLE ✔

**Цель.** Закрыть ревью 3 (расширяемость): таблицы вместо copy-paste,
`CFG_ENUM` для canBitrate, дублирование `readUnsignedLE`, паттерн
«old->write->new->memcmp».

**Файлы.**
- `components/04_network/communication/src/CommModule.cpp` (диспатч команд)
- `components/04_network/server/src/OtaApi.cpp` + `ServerModule.cpp` (URI)
- `components/04_network/server/src/StaticHandler.cpp` (MIME)
- `components/01_core/common/src/FieldRegistry.cpp` + `ConfigStore.cpp` (readLE)
- `components/01_core/common/include/fields/TwaiFields.inc` (canBitrate)
- `components/01_core/common/src/FieldRegistry.cpp` (CFG_ENUM валидация)

**Детали.**
- **Диспатч команд** (`CommModule.cpp:86-171`): switch по msgType, ручная
  валидация длины в каждом case. Таблица:
  ```cpp
  struct CmdDesc { uint16_t type; size_t minLen; void (CommunicationModule::*fn)(const uint8_t*, size_t, int); };
  static constexpr CmdDesc kCmds[] = { {kMsgTypeRequest, 5, &...}, ... };
  ```
  + поиск + вызов; `minLen` вместо ручных проверок в case. Формат payload
  (парсинг uid) — отдельными маленькими методами. Осторожно: `PARAM_SET`
  minLen=2 (value опционален -> обработка как сейчас), `PARAM_REQUEST` ==2
  строго -> `minLen=2, exact=true`? Проще: таблица `{type, minLen, exact, fn}`.
  Реализация на месте — цель: убрать дублирование проверок, порядок
  дефолтного case сохранить.
- **URI-роутинг** (`OtaApi.cpp:73-107` + `ServerModule.cpp:54-61`): 3 почти
  одинаковых `httpd_uri_t` в OtaApi + wildcard. Таблица
  `{uri, method, handler, user_ctx}` + цикл регистрации; ПОРЯДОК (wildcard
  последним) — в комментарии к таблице (сохранить как сейчас: конкретные
  раньше wildcard; в ServerModule static регистрируется последним). Таблица
  локальная в OtaApi.cpp (static) — общий реестр URI по всем модулям
  перебор (слои не знают друг друга) — ограничиться OtaApi.
  `max_uri_handlers = 32` хардкод (`ServerModule.cpp:35`) — заменить на
  константу `kMaxUriHandlers` рядом с таблицами (семантика та же).
- **MIME** (`StaticHandler.cpp:49-69`): if-цепочка -> static таблица
  `{{".html","text/html"}, ...}` + линейный поиск (N=7 — бинарный поиск не
  нужен).
- **CFG_ENUM для canBitrate** (`TwaiFields.inc:10`): сейчас `CFG_UINT,
  125000..1000000` -> принимает 125001. Валидация CFG_ENUM уже в
  FieldRegistry (`FieldRegistry.cpp:107`: enum/bool читаются как
  беззнаковые с min..max) — это ДИАПАЗОН, а не список значений! Т.е.
  CFG_ENUM сегодня = uint с диапазоном. Чтобы запретить 125001, нужен
  СПИСОК. Варианты: (а) хранить canBitrate как ИНДЕКС (0..3) в enum-поле ->
  ломает конфиг/JSON (значения 125000..1000000 в JSON) — фронт ещё не
  написан! Можно индекс... но и в `canBitrate` уже могут смотреть люди.
  (б) добавить в FieldMeta/CFG_ENUM поддержку «allowed values» — большой
  механизм (нужно поле со списком в метаданных). (в) ПРОЩЕ: валидация
  списка В `validateField` по конкретному имени? Хардкод — против
  декларативности. РЕШЕНИЕ НА МЕСТЕ: наиболее практично (б) с минимализмом —
  добавить в FieldMeta не обязательно; можно использовать существующий
  механизм: `validator == CFG_ENUM` + min/max + новый чек в validateField:
  «значение должно быть из набора» — набор привязать к validator'у через
  таблицу в FieldRegistry.cpp по имени поля? Снова хардкод. Честный путь:
  **перечисление в .inc невозможно** (X-macro не тянет списки).
  Компромисс, который реально делается: ПУНКТ ОТЛОЖИТЬ или сделать
  простой вариант: `CFG_ENUM` + min=125000 max=1000000 + проверка
  `value` через `isValidBitrate`-хелпер в HardwareConfig — но validateField
  в слое 01 не должен знать про TWAI-битрейты (нарушение слоёв).
  ВЫВОД: в этом шаге сделать МИНИМУМ: убрать дублирование
  `kAllowedBitrates` (сейчас `J1939System.cpp:24` + откат `:97-99` +
  `:212-219`) — перенести список в один хелпер
  `Hw::isValidBitrate()` в HardwareConfig.h (constexpr), использовать везде;
  полный CFG_ENUM со списком — ОТЛОЖИТЬ (записать в план как «вне объёма,
  требует расширения FieldMeta»). Так ревью-пункт закрывается частично и
  честно.
- **readLE дублирование**: `FieldRegistry.cpp:23-31` vs `ConfigStore.cpp:23-31`
  — вынести в общий заголовок (например, `components/01_core/common/include/ByteOrder.h`
  с `inline uint64_t readUnsignedLE(const uint8_t*, size_t)` и signed-версией);
  оба .cpp включают и удаляют локальные копии. Проверить REQUIRES
  config_store -> common (есть: `REQUIRES common` в CMakeLists config_store).
- **Паттерн old->write->new->memcmp**: `CommModule.cpp:136-152` и
  `ConfigStore.cpp:128-150`. Общий хелпер `FieldWriteResult
  writeFieldDetectChange(uid, data, len, owner, bool& changed)` в
  FieldRegistry (там же, где writeField): внутри — под одним локом (рекурсивный
  мьютекс — ок) read old -> write -> read new -> memcmp -> changed.
  Оба call site переезжают на него. Это снимает и дублирование, и потенциальную
  гонку между read и write (сейчас read/write под общим локом? В CommModule
  readField/writeField вызываются ПОДРЯД — каждый берёт свой лок внутри —
  между ними возможно вмешательство! Правка устраняет и это).
- **Упаковка uid LE (4 места в CommModule)**: `payload[0]=uid&0xFF;
  payload[1]=uid>>8` — хелпер `writeU16LE(uint8_t*, uint16_t)` в ByteOrder.h
  (вместе с readLE). Мигрировать 4 места + чтение uid в onIncomingPacket
  (2 места) на `readU16LE`.

**Правки.** Таблицы (команды, URI, MIME), ByteOrder.h + readLE/writeU16LE
миграция, writeFieldDetectChange, Hw::isValidBitrate устранение дублей,
`kMaxUriHandlers`. CFG_ENUM — минимум (см. выше).

**Подводные камни.**
- Таблица команд с метод-указателями: методы должны иметь сигнатуру
  `(const uint8_t* payload, size_t len, int sockfd)` — рефакторинг текущих
  case-тел в методы; `j1939_.onClientRequest` остаётся как есть.
- Порядок URI регистрации сохранить (диаграмма в ServerModule.cpp:54-58).
- ByteOrder.h header-only inline — не плодить TU.
- `writeFieldDetectChange` — читать old/новый ПОД ОДНИМ локом: использовать
  существующий рекурсивный `adataMutex` (FieldRegistry уже его держит) —
  сделать приватный `writeFieldLocked` или полагаться на рекурсивность
  (текущий `writeField` сам лочит — рекурсивный мьютекс позволяет держать
  внешний: обернуть вызовы в `std::lock_guard` на `mutex_`? `mutex_` — private
  член FieldRegistry — ок, внутри класса).

**Проверка.** `idf.py build` + host-тесты (FieldRegistry меняется — тесты
на writeFieldDetectChange добавить; ByteOrder — тесты roundtrip).

**Коммит.** `ARCH_FIX2 шаг 14: таблицы команд/URI/MIME, ByteOrder.h (readLE/writeU16LE), writeFieldDetectChange, Hw::isValidBitrate` + push.

**СТОП.**

---

## Примечания после завершения

- Когда все шаги отмечены: закоммитить обновлённый план (галочки) и удалить
  либо оставить в docs/ — по аналогии с прошлым ARCH_FIX план удалялся после
  закрытия (`2dc5d1b`). Решение на пользователе.
- Отложенные пункты ревью (вне объёма этого плана): `.h`->`.hpp`, полный
  CFG_ENUM со списками, `std::optional`-стиль, полное устранение дублирования
  доменов AppData (4 include-прохода — требует X-macro редизайна), C++26 в
  host-тестах (gcc в WSL не тянет 26 — максимум 23).
