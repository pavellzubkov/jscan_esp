# STEP-07 — `04_network/netctrl` + переписывание `main` + удаление старых компонентов

> Контекст: свежая сессия. STEP-01..06 выполнены, все новые компоненты
> компилируются, проект собирается со старым `main`. Этот шаг **переключает**
> проект на новую архитектуру: NetworkController, BootManager-запуск, удаление
> старых плоских компонентов. Проверка: `idf.py reconfigure && idf.py build`
> (и, при наличии железа, прошивка + monitor — подробнее в STEP-08).

## Цель

1. `04_network/netctrl/NetworkController` — владеет `WifiApModule` и
   `ServerModule`, управляет их жизненным циклом (аналог TEMP_PID).
2. `main/main.cpp` переписан: AppContext + BootManager + `REGISTER_MODULE`.
3. Старые компоненты удалены: `_common`, `esp_wifi_start`, `esp_fs_start`,
   `esp_j1939`.
4. Проект собирается целиком на новой архитектуре.

## 1. `netctrl`

### Структура

```
components/04_network/netctrl/
  CMakeLists.txt
  include/NetworkController.hpp
  src/NetworkController.cpp
```

### `include/NetworkController.hpp`

```cpp
#pragma once
#include "AppContext.h"
#include "wifi/WifiApModule.hpp"
#include "server/ServerModule.hpp"

// Координатор сети: владеет Wi-Fi (AP) и веб-сервером (HTTP+WS).
class NetworkController {
public:
    explicit NetworkController(AppContext* ctx);
    ~NetworkController();

    esp_err_t begin();   // wifi.begin() → server.begin()
    void      stop();    // server.stop() → wifi.stop()

private:
    AppContext* ctx_;
    WifiApModule wifi_;
    ServerModule server_;
};
```

### `src/NetworkController.cpp`

- `begin()`: `esp_event_loop_create_default()` вызывается в main (не здесь);
  `wifi_.begin()` → если OK → `server_.begin()`.
- Если wifi/server не стартовали — логировать `ESP_LOGW` и вернуть ошибку
  (деградация: J1939-скан работает, сеть недоступна). `critical=false` в
  BootManager позволит системе жить без сети.
- `stop()`: обратный порядок.

### `CMakeLists.txt`

```cmake
idf_component_register(
    SRC_DIRS .
    INCLUDE_DIRS .
    REQUIRES common wifi server freertos log
)
```

## 2. `main/main.cpp` — переписать

Источник паттерна: `E:\Projects\Embedded\ESP32\Temp_pid\TEMP_PID\main\main.cpp`.

```cpp
#include "AppContext.h"
#include "BootManager.h"
#include "ConfigStore.h"
#include "CommModule.h"
#include "NetworkController.hpp"
#include "J1939System.h"
#include "esp_event.h"

template <typename Module, typename... Args>
static esp_err_t makeModule(AppContext* ctx, Args&&... args) {
    Module* m = new Module(ctx, std::forward<Args>(args)...);
    esp_err_t err = m->begin();
    if (err != ESP_OK) { delete m; }
    return err;
}

#define REGISTER_MODULE(name, Type, priority, critical, ...) \
    boot.add({name, [](AppContext* c) { return makeModule<Type>(c, ##__VA_ARGS__); }, priority, critical})

extern "C" void app_main() {
    ESP_ERROR_CHECK(esp_event_loop_create_default());   // системные события (WiFi)
    ESP_ERROR_CHECK(esp_netif_init());                   // сетевой интерфейс

    AppContext ctx;
    ESP_ERROR_CHECK(ctx.initEventLoop());

    BootManager boot;

    REGISTER_MODULE("config",  ConfigStore,         10, true);
    REGISTER_MODULE("comm",    CommunicationModule, 20, false);
    REGISTER_MODULE("netctrl", NetworkController,   40, false);
    REGISTER_MODULE("j1939",   J1939System,         70, false);

#undef REGISTER_MODULE

    esp_err_t overall = boot.startAll(&ctx);
    if (overall != ESP_OK) {
        ESP_LOGW("MAIN", "Some modules failed — degraded mode");
    }

    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
```

Примечания:
- `esp_netif_init()` можно вызывать и в WifiApModule (идемпотентно), но явно в
  main — понятнее.
- В конструкторах новых модулей нет AppState — только `AppContext*`.
- Порядок важен: `config` (10) раньше `netctrl` (40), чтобы AP-параметры уже
  были загружены из JSON.

## 3. `main/CMakeLists.txt`

```cmake
idf_component_register(
    SRCS "main.cpp"
    REQUIRES common config_store communication netctrl j1939_system
)

spiffs_create_partition_image(storage ../spiffs_image FLASH_IN_PROJECT)
```

(Имена компонентов: `common`, `config_store`, `communication`, `netctrl`,
`j1939_system` — по имени каталога в слое. Убедиться, что пути в REQUIRES
совпадают с реальными именами компонентов после `idf.py reconfigure`.)

## 4. Удалить старые компоненты

Удалить каталоги:
- `components/_common/`
- `components/esp_wifi_start/`
- `components/esp_fs_start/`
- `components/esp_j1939/`

Проверить, что в `components/` остались только слои:
```
components/01_core/ 02_hardware/ 03_systems/ 04_network/ 05_storage/
```

После удаления:
- `main/CMakeLists.txt` больше не ссылается на `_common`/`esp_wifi_start`/...;
- `dependencies.lock`/`managed_components` пересоздадутся (`idf.py reconfigure`);
- cjson теперь нужен только `config_store` (уже объявлен в его `idf_component.yml`).

## Критерий готовности

1. `idf.py reconfigure` — без ошибок; в выводе видны новые компоненты, нет
   упоминаний удалённых.
2. `idf.py build` — **успех без предупреждений**.
3. В `build/compile_commands.json` нет путей в удалённые компоненты.

## Возможные грабли (проверить)

- **Имя компонента `server`** может конфликтовать с чем-то системным? Нет, имя
  определяется каталогом. Но убедиться, что `REQUIRES server` в netctrl резолвится
  в `components/04_network/server`.
- **Двойной mount SPIFFS**: ConfigStore монтирует первой; ServerModule использует
  `SpiffsService::mount()` (идемпотентен) — проверить отсутствие `ESP_ERR_INVALID_STATE`.
- **Отсутствие event loop**: `ctx.initEventLoop()` создаёт свой loop; системный
  default loop для WiFi — `esp_event_loop_create_default()` в начале `app_main`.
- Если `idf.py build` упадёт на удалённых include — удалить остатки ссылок в
  `main.cpp` и проверить `components/*/CMakeLists.txt`.