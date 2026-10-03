# План ARCH_FIX3 — отложенные пункты ревью

Источник: «Отложенные пункты ревью» в конце `docs/PLAN/ARCH_FIX2_PLAN.md`
(строки 1330-1333) + `docs/ARCHITECTURE_REVIEW.md`.
Все пути и номера строк — по состоянию на коммит-базу этого плана; если
разошлось, ищи по фрагменту кода/контексту.

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

Успешный вывод: `checks=N failures=0` (N — база на момент начала плана;
уточнить фактическое число первым запуском до правок).
`rm -rf build/host` обязателен, если менялся состав файлов/стабы в
`tests/host/CMakeLists.txt` или сам CMakeLists; иначе можно без него.

---

# Сводная таблица шагов

| # | Шаг | Статус |
|---|-----|--------|
| 0 | Коммит плана (документы) | ☑ |
| 1 | Ренейминг `.h` -> `.hpp` (21 файл, ~84 include) | ☑ |
| 2 | `Domains.inc` — единый источник списка доменов | ☑ |
| 3 | CFG_ENUM: список разрешённых значений для canBitrate | ☑ |
| 4 | Host-тесты: C++20 -> C++23 | ☐ |

**Не делаем (вне объёма, зафиксировано):** `std::optional`-модернизация API
(стиль, ~12 файлов, без влияния на wire — ревью само помечает как
«модернизация стиля»); C++26 в host-тестах (g++ 11.4 в WSL Ubuntu-22.04 ->
максимум `-std=gnu++2b` (частичный C++23); полный паритет недостижим без
смены компилятора/дистрибутива — шаг 4 закрывает возможность частично);
BootManager-указатели модулей для shutdown (закрыто шагом 12 ARCH_FIX2 —
join через stop-флаг).

---

## Шаг 0. Коммит плана ✔

**Цель.** Положить в репозиторий `docs/PLAN/ARCH_FIX3_PLAN.md`.

**Файлы.** `docs/PLAN/ARCH_FIX3_PLAN.md`.

**Правки.** Нет (только документ).

**Проверка.** `git status` — файл staged. Сборка не нужна.

**Коммит.** `DOCS: план ARCH_FIX3 (отложенные пункты ARCH_FIX2)` + push.

**СТОП.**

---

## Шаг 1. Ренейминг `.h` -> `.hpp` ✔

**Цель.** Консистентность расширений: 21 собственный `.h` при 12 уже
существующих `.hpp` (слой 04 network полностью `.hpp`). Механика без
логики.

**Файлы (21, `git mv`).**
- `components/01_core/common/include/` (13): `AppContext.h`,
  `AppData.h`, `AppEvents.h`, `AppTypes.h`, `BootManager.h`, `ByteOrder.h`,
  `EventManager.h`, `FieldRegistry.h`, `HardwareConfig.h`, `J1939Proto.h`,
  `LogicUtils.h`, `RaiiGuards.h`, `SystemTiming.h`
- `components/02_hardware/twai/include/`: `TwaiDriver.h`
- `components/03_systems/j1939_system/include/` (4): `J1939Decoder.h`,
  `J1939System.h`, `J1939TransportProtocol.h`, `SnapshotAccumulator.h`
- `components/03_systems/system_status/include/`: `SystemStatusModule.h`
- `components/04_network/communication/include/`: `CommModule.h`
- `components/05_storage/config_store/include/`: `ConfigStore.h`

**Правки.**
1. `git mv Name.h Name.hpp` для каждого из 21 файла.
2. Заменить `#include "Name.h"` -> `#include "Name.hpp"` по всему коду
   (~84 строки; составить список grep'ом **до** правки и сверить счёт после).
   Точная команда-сверка до правки:
   `grep -rn '#include ".*\.h"' components main tests/host/main.cpp`
   — после правки должны остаться только стабы (`tests/host/stubs`) и
   вендор `esp_littlefs`.
3. Комментарий `tests/host/CMakeLists.txt:29` (`TwaiDriver.h` -> `.hpp`).
4. `docs/PROTOCOL-J1939.md` — упоминание `J1939Proto.h` (1 место —
   прескриптивный док, обновить). Исторические доки (`ARCH_FIX2_PLAN.md`,
   `ARCHITECTURE_REVIEW.md`) не трогать.

**Не трогать.**
- `tests/host/stubs/**` — `esp_log.h`, `freertos/*` и пр. обязаны совпадать
  с именами IDF.
- `components/05_storage/esp_littlefs/**` — вендор.
- `*.inc` — расширение не `.h`.

**Подводные камни.**
- CMakeLists компонентов имен заголовков не содержат (SRCS только `.cpp`;
  `.clangd` читает compile_commands) — правок CMake не требуется.
- Include-гарды — `#pragma once`, не меняются.
- Замены делать с точными кавычками `"Name.h"`, чтобы не зацепить
  `esp_log.h` и др. — имена уникальны, коллизий нет.

**Проверка.** `idf.py build` + host-тесты (состав CMake не менялся —
`rm -rf build/host` не обязателен, но прогнать штатно; `checks=N failures=0`).

**Коммит.** `ARCH_FIX3 шаг 1: .h -> .hpp (21 файл, 84 include)` + push.

**СТОП.**

---

## Шаг 2. `Domains.inc` — единый источник списка доменов ✔

**Цель.** Закрыть отложенный пункт ARCH_FIX2 («полное устранение
4 `#include`-проходов доменов AppData») + ревью §6 (п.6): список доменов
сейчас в **5 копиях** — enum `AppTypes.h:36-42`, 4 include-цепочки
`AppData.h:31-34 / 47-50 / 91-102 / 161-164`, мёртвый
`DataFields.inc:24-27`. Новый домен = правка в 5 местах; забытый include
ничем не ловится.

**Дизайн — вариант A (мультимод-мастер-файл).** Один файл с литеральными
директивами (директивы препроцессора не могут рождаться из расширения
макроса — единственный легальный способ свернуть цепочки), включается N раз
с разными определениями `DOMAIN_ENTRY`/`DATA_FIELD`.

**Файлы.**
- `components/01_core/common/include/DataFields.inc` -> `git mv` ->
  `Domains.inc` (переписать)
- `components/01_core/common/include/AppTypes.h` (:34-42)
- `components/01_core/common/include/AppData.h`
  (:10-20, :31-34, :47-50, :74-103, :120, :123, :161-164)
- `AGENTS.md` (локально, без коммита — строка про «корневой DataFields.inc»)

**Дизайн `Domains.inc` (черновик).**
```cpp
// Единый список доменов системы. Включается с разными определениями
// DOMAIN_ENTRY / DATA_FIELD:
//   AppTypes.h — генерация enum FieldDomain (DOMAIN_ENTRY(x) -> x,);
//   AppData.h  — 4 прохода (struct / defaults / meta / UID), DOMAIN_ENTRY пуст.
// FIELD_DOMAIN подмешивается для meta-прохода g_fieldMeta[].
// Порядок блоков определяет порядок enum, членов AppData и g_fieldMeta[]
// (enum: WIFI..SYSTEM, PROTOCOL дописывается вручную в AppTypes.h).
DOMAIN_ENTRY(WIFI)
#undef FIELD_DOMAIN
#define FIELD_DOMAIN FieldDomain::WIFI
#include "WifiFields.inc"
#undef FIELD_DOMAIN

DOMAIN_ENTRY(TWAI)
#undef FIELD_DOMAIN
#define FIELD_DOMAIN FieldDomain::TWAI
#include "TwaiFields.inc"
#undef FIELD_DOMAIN

DOMAIN_ENTRY(SNAPSHOT)
#undef FIELD_DOMAIN
#define FIELD_DOMAIN FieldDomain::SNAPSHOT
#include "SnapshotFields.inc"
#undef FIELD_DOMAIN

DOMAIN_ENTRY(SYSTEM)
#undef FIELD_DOMAIN
#define FIELD_DOMAIN FieldDomain::SYSTEM
#include "SystemFields.inc"
#undef FIELD_DOMAIN
```
Интеллисент-ветку `__INTELLISENSE__` из старого `DataFields.inc`
перенести/адаптировать (fallback-определение `DATA_FIELD`).

**Правки.**
1. `AppTypes.h` — enum генерируется:
   ```cpp
   enum class FieldDomain : uint8_t {
   #define DOMAIN_ENTRY(x) x,
   #define DATA_FIELD(...)        // пусто: поля в enum не входят
   #include "Domains.inc"
   #undef DOMAIN_ENTRY
   #undef DATA_FIELD
     PROTOCOL   // sentinel: пишущий по протоколу (не владеет runtime-полями)
   };
   ```
2. `AppData.h` — все 4 цепочки (`:31-34`, `:47-50`, `:91-102`, `:161-164`)
   -> `#include "Domains.inc"`. В каждом проходе определить
   `#define DOMAIN_ENTRY(...)` как пусто (+ `DATA_FIELD` как сейчас:
   `type name{};` / `data.name=(def);` / инициализатор FieldMeta с
   `FIELD_DOMAIN` / `constexpr uid`). Блоки `FIELD_DOMAIN` в AppData.h
   больше не нужны (переехали в Domains.inc) — удалить `:91-102`.
   Комментарий-таблицу `:14-19` заменить на ссылку на Domains.inc.
3. Убрать макрос `DATA_FIELD_DOMAIN` (используется только в старом
   `DataFields.inc` — grep перед правкой подтверждает).
4. Обновить строки-ссылки: `AppData.h:120` (текст static_assert),
   `AppData.h:123`, `AppTypes.h:34`.

**Подводные камни.**
- **Порядок обязан сохраниться** (WIFI, TWAI, SNAPSHOT, SYSTEM; PROTOCOL —
  последним): от enum зависит `static_cast<int>` в логах
  `FieldRegistry.cpp`; от порядка include — layout AppData/config-blob и
  `g_fieldMeta[]`.
- Include-path: `#include "WifiFields.inc"` (без `fields/`) — резолвится
  через `INCLUDE_DIRS "include/fields"` (common/CMakeLists.txt:7) и
  `target_include_directories` host-тестов — оставить форму как сейчас.
- В «пустых» проходах `FIELD_DOMAIN` определён (из Domains.inc), но не
  используется экспансией `DATA_FIELD` — безвредно; после последнего блока
  всегда `#undef`.
- UID не меняются (хэш от имени полей) -> `static_assert(checkFieldUids())`
  и host-тесты обязаны пройти **без правок тестов** — это и есть проверка
  консистентности.
- `AppTypes.h` — нижний слой: в enum-проходе `DATA_FIELD` пуст -> ветка
  не тянет HardwareConfig/SystemTiming (они и не нужны: `fields/*.inc`
  при пустом `DATA_FIELD` не вычисляют аргументы).

**Проверка.** `idf.py build` + host-тесты (состав CMake не менялся —
прогнать; `checks` должен совпасть с базой до шага).

**Коммит.** `ARCH_FIX3 шаг 2: Domains.inc — единый источник доменов (5 копий -> 1)` + push.

**СТОП.**

---

## Шаг 3. CFG_ENUM: список разрешённых значений для canBitrate ✔

**Цель.** Закрыть отложенный пункт «полный CFG_ENUM со списками» + баг
ревью: `canBitrate` объявлен `CFG_UINT` + диапазон 125000..1000000 ->
**принимает 125001** (`valueInRange` проверяет только min..max).
Wire-формат не меняется (значение числовое; NACK `err=2` range уже
покрывает отказ — `PROTOCOL-J1939.md:79`).

**Файлы.**
- `components/01_core/common/include/HardwareConfig.h`
- `components/01_core/common/include/fields/TwaiFields.inc` (:10)
- `components/01_core/common/src/FieldRegistry.cpp` (`valueInRange`)
- `tests/host/main.cpp` (тесты)

**Детали (что выяснено).**
- `CFG_ENUM` в `.inc` не используется ни разу; валидация —
  `FieldRegistry.cpp:70-100` (`valueInRange`): для enum идёт ветка
  беззнакового чтения + только диапазон.
- Единственный кандидат на список сейчас — `canBitrate`; список уже
  существует (`Hw::isValidBitrate`, `HardwareConfig.h:16-22`; дублей в
  call site нет после шага 14 ARCH_FIX2).
- **Дизайн: таблица uid->список в FieldRegistry.cpp.** Вариант
  «FieldMeta +2 поля» отклонён — потребовал бы расширения сигнатуры
  `DATA_FIELD` во всех 4 `.inc` x 4 макроса. Таблица приватна для слоя 01;
  wire/ConfigStore/генератор схемы не трогаются.
- `ConfigStore` не требует правок: `jsonToWireValue` уже группирует
  CFG_ENUM с CFG_UINT (`ConfigStore.cpp:56-57`), валидация централизована
  в `writeField`.
- Сценарий `config.json`: значение 125001 теперь **отклоняется при
  загрузке** (поле остаётся на дефолте) — желаемое поведение.

**Правки.**
1. `HardwareConfig.h`: вынести массив наружу из функции:
   ```cpp
   constexpr uint32_t kAllowedBitrates[] = {125000, 250000, 500000, 1000000};
   constexpr bool isValidBitrate(uint32_t br) { /* цикл по kAllowedBitrates */ }
   ```
   (`FieldRegistry.cpp` уже тянет `HardwareConfig.h` через `AppData.h` —
   новый include не нужен; host-тесты имеют стаб `driver/gpio.h`.)
2. `TwaiFields.inc:10`: `CFG_UINT` -> `CFG_ENUM` (min/max оставить
   125000..1000000 как внешнюю границу; в @description указать источник
   списка `Hw::kAllowedBitrates`).
3. `FieldRegistry.cpp`:
   ```cpp
   // Списки разрешённых значений для CFG_ENUM (uid -> массив).
   struct EnumList { uint16_t uid; const uint32_t* values; size_t count; };
   static constexpr EnumList kEnumLists[] = {
       { canBitrate_UID, Hw::kAllowedBitrates,
         sizeof(Hw::kAllowedBitrates) / sizeof(uint32_t) },
   };
   ```
   В `valueInRange`, целочисленная ветка **после** проверки диапазона:
   если `validator == CFG_ENUM` и для `meta->uid` есть запись в
   `kEnumLists` — проверить членство (сравнение значения со списком);
   нет записи -> поведение как раньше (совместимость).
4. Host-тесты (`test_registry` в `tests/host/main.cpp`):
   - `125001` (в диапазоне, вне списка) -> `OUT_OF_RANGE`;
   - `250000` -> `OK`.
   Существующие проверки не ломаются: 500000/1000000 — в списке; `100` —
   вне диапазона; длины отсеиваются раньше (`:95`).

**Подводные камни.**
- `writeFieldScalar(canBitrate_UID, ...)` в откате `J1939System.cpp:110` —
  значение после `isValidBitrate` уже валидно -> членство пройдёт.
- `writeFieldString(canBitrate_UID, ...)` -> `BAD_LENGTH` (CFG_ENUM не в
  строковом наборе) — не ломается.
- Порядок проверок в `valueInRange`: длина -> диапазон -> членство
  (иначе tooShort/tooLong дадут другой статус).
- `gen_schema.ts` / фронтенд (вне репо): CFG_ENUM в схеме уже есть как
  тип; вывод списка значений генератором — за рамками шага (зафиксировано).
- `twaiState` (readonly runtime, 0..3) — не трогаем (protocol-записи
  на него нет).

**Проверка.** `idf.py build` + host-тесты (`rm -rf build/host` не обязателен
— состав не менялся; `checks=N failures=0`, N больше базы на 2 новых).

**Коммит.** `ARCH_FIX3 шаг 3: CFG_ENUM — таблица uid->allowed values, canBitrate по списку, host-тесты` + push.

**СТОП.**

---

## Шаг 4. Host-тесты: C++20 -> C++23

**Цель.** Шаг к паритету с таргетом `gnu++26` (решение пользователя: 23;
полный 26 — won't-do). **Проверено в WSL:** cmake 3.22.1 (нужно >= 3.20
для `CXX_STANDARD 23`), g++ 11.4.0 (CMake выдаст `-std=gnu++2b`).

**Файлы.**
- `tests/host/CMakeLists.txt`
- `AGENTS.md` (локально, без коммита)

**Правки.**
1. `tests/host/CMakeLists.txt:1`
   `cmake_minimum_required(VERSION 3.16)` -> `3.20`.
2. `:4` `set(CMAKE_CXX_STANDARD 20)` -> `23`.
3. `AGENTS.md`: «Host-тесты (без IDF, C++20)» -> C++23.

**Подводные камни.** Если cmake/gcc не сдвинутся — откат обеих строк и
зафиксировать в плане «оставлено C++20» (на тестах фичи 20+ не
используются, риск только в CMake-конфигурации). Обязателен
`rm -rf build/host` (меняется конфиг CMake).

**Проверка.** Host-тесты из WSL-команды: `checks=N failures=0`.
Прошивку собирать не обязательно (файл только в tests).

**Коммит.** `ARCH_FIX3 шаг 4: host-тесты C++20 -> C++23 (cmake 3.20+, gnu++2b)` + push.

**СТОП.**

---

## Примечания после завершения

- Когда все шаги отмечены: закоммитить обновлённый план (галочки) и
  удалить либо оставить в docs/ — по аналогии с прошлым ARCH_FIX план
  удалялся после закрытия (`2dc5d1b`). Решение на пользователе.
- Итог за бортом (зафиксировано): `std::optional`-стиль; C++26 в тестах;
  BootManager-указатели (закрыто шагом 12 ARCH_FIX2).