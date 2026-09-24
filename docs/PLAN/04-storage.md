# STEP-04 — `05_storage` (spiffs_service + config_store)

> Контекст: свежая сессия. STEP-01..03 выполнены, проект собирается. Этот шаг
> добавляет слой хранения: обёртку над SPIFFS и конфиг-стор с JSON-персистенцией.
> Модули **ещё не подключаются** в main (до STEP-07), но компилируются.
> Проверка: `idf.py reconfigure && idf.py build`.

## Цель

- `05_storage/spiffs_service` — обёртка над `esp_spiffs` (порт из старого
  `esp_fs_start/_fs_spiffs_sd.cpp` + методы read/write/exists).
- `05_storage/config_store` — `ConfigStore`: загрузка/сохранение `AppConfig`
  в `/spiffs/config.json` (cJSON), дебаунс-сохранение, публикация
  `CONFIG_CHANGED`. **Единственный писатель `ctx->config`.**

## Структура

```
components/05_storage/
  spiffs_service/
    CMakeLists.txt
    include/SpiffsService.hpp
    src/SpiffsService.cpp
  config_store/
    CMakeLists.txt
    idf_component.yml
    include/ConfigStore.h
    src/ConfigStore.cpp
```

## 1. `spiffs_service`

### `include/SpiffsService.hpp`

```cpp
#pragma once
#include "esp_err.h"
#include <cstddef>

// Обёртка над esp_spiffs: mount/read/write/exists/remove.
class SpiffsService {
public:
    SpiffsService(const char* basePath, const char* partitionLabel, bool formatOnFail);
    ~SpiffsService();

    esp_err_t mount();        // идемпотентно: повторный mount возвращает ESP_OK
    void      unmount();

    bool      fileExists(const char* relPath) const;   // relPath без base
    esp_err_t readFile(const char* relPath, char** outBuf, size_t* outLen) const;   // malloc
    esp_err_t writeFile(const char* relPath, const void* data, size_t len) const;
    esp_err_t removeFile(const char* relPath) const;

private:
    char basePath_[32];
    char partitionLabel_[32];
    bool formatOnFail_;
    bool mounted_ = false;
};
```

### `src/SpiffsService.cpp`

- Порт mount из `esp_fs_start/_fs_spiffs_sd.cpp` (`esp_vfs_spiffs_register` с
  `format_if_mount_failed`); логировать total/used через `esp_spiffs_info`.
- `readFile`: `fopen(fullPath,"rb")`, `fseek/ftell` → malloc + `fread` + `fclose`.
- `writeFile`: `fopen(fullPath,"wb")` + `fwrite` + `fclose` (атомарность на этом
  этапе не требуется; конфиг маленький).
- `fileExists`: `access(fullPath, F_OK) == 0`.
- Префикс base path: `snprintf(full, sizeof full, "%s/%s", basePath_, relPath)`.

### `CMakeLists.txt`

```cmake
idf_component_register(
    SRC_DIRS .
    INCLUDE_DIRS .
    REQUIRES spiffs vfs freertos log
)
```

## 2. `config_store`

### `include/ConfigStore.h`

```cpp
#pragma once
#include "AppContext.h"
#include "spiffs_service/SpiffsService.hpp"
#include <cstdint>

// Хранилище конфигурации. Единственный писатель ctx->config (load/defaults).
// Монтирует SPIFFS (если не смонтирован), читает /spiffs/config.json при старте,
// сохраняет с дебаунсом при изменении, публикует CONFIG_CHANGED.
class ConfigStore {
public:
    explicit ConfigStore(AppContext* ctx);
    ~ConfigStore();

    esp_err_t begin();   // монтирует FS, грузит конфиг, создаёт таск автосейва

    // Загрузить значение поля в ctx->config и сохранить (для будущих команд
    // протокола). Сейчас только чтение.
    void setAndSave(const AppConfig& next);   // не используется в этом этапе — задел

private:
    static constexpr const char* kConfigPath = "/config.json";   // в /spiffs

    AppContext* ctx_;
    SpiffsService fs_;

    void loadFromFs();       // читает JSON → ctx->config (нет файла → дефолты + save)
    void saveToFs();         // ctx->config → JSON → fs_

    static void autoSaveWrapper(void* p);
    void autoSaveLoop();     // раз в ~1 с: если dirty → saveToFs()
    bool dirty_ = false;
    TaskHandle_t task_ = nullptr;
};
```

### `src/ConfigStore.cpp`

- `begin()`: `fs_.mount()`; `loadFromFs()`; создать таск `cfg_autosave`
  (стек 4096, prio 3) с циклом: `vTaskDelay(1000)` → если `dirty_` → `saveToFs()`.
- `loadFromFs()`: `fs_.readFile(kConfigPath, &buf, &len)`; если нет файла —
  оставить дефолты (уже в `AppConfig`) и `dirty_ = true` (сохранить при первом
  тике); парсить cJSON: `ap_ssid`, `ap_password`, `ap_channel`, `max_sta_conn`,
  `snapshot_interval_ms`, `snapshot_ttl_ms`, `max_tracked_pgns`; скопировать в
  `ctx->config` (только валидные значения, диапазоны: channel 1–14,
  interval 100–1000, ttl 500–10000, maxTracked 16–256).
- `saveToFs()`: собрать cJSON-объект из `ctx->config`, `cJSON_PrintUnformatted`,
  `fs_.writeFile`, `free`; снять `dirty_`; залогировать `ESP_LOGI`.
- Публикация `CONFIG_CHANGED` — задел: при изменении (пока нет источника
  изменений) вызывать нечего; метод `setAndSave` помечен как будущий.
- Обработка ошибок: при сбое чтения JSON — логировать `ESP_LOGE`, не падать
  (дефолты).

### `CMakeLists.txt`

```cmake
idf_component_register(
    SRC_DIRS .
    INCLUDE_DIRS .
    REQUIRES common spiffs_service espressif__cjson freertos log
)
```

### `idf_component.yml`

```yaml
dependencies:
  espressif/cjson: "^1.7.19"
```

## Критерий готовности

1. `idf.py reconfigure` (пересоздаст `managed_components`/`dependencies.lock`).
2. `idf.py build` — успех (новые `spiffs_service`, `config_store` компилируются;
   старые компоненты не тронуты).
3. В логе при будущем подключении (STEP-07) конфиг читается/пишется в
   `/spiffs/config.json`.

## Замечания

- Конфиг маленький и некритичный: без атомарной записи (`.bak`/rename) на этом
  этапе можно; если появится желание — добавить, как в TEMP_PID
  (`ConfigStore::save` делает `.bak`).
- Не монтировать SPIFFS дважды: `SpiffsService::mount()` идемпотентен, а
  `ServerModule` (STEP-05) использует уже смонтированную ФС.
- Старый `WEB_CONFIG_FILENAME`/`InitConfig` из `_common` больше не нужен.