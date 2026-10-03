#include "ConfigStore.h"
#include "AppData.h"
#include "LogicUtils.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static const char* TAG = "ConfigStore";

// ============================================================
// Вспомогательные функции (сериализация little-endian)
// ============================================================

static uint64_t readUnsignedLE(const uint8_t* p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static int64_t readSignedLE(const uint8_t* p, size_t n) {
    uint64_t v = readUnsignedLE(p, n);
    if (n < 8) {
        bool neg = v & (uint64_t(1) << (8 * n - 1));
        if (neg) v |= ~((uint64_t(1) << (8 * n)) - 1);
    }
    return static_cast<int64_t>(v);
}

// ============================================================
// JSON-слой: значение JSON → wire (PROTOCOL §2) → FieldRegistry
// ============================================================

static bool jsonToWireValue(const FieldMeta& meta, const cJSON* item,
                            uint8_t* out, size_t cap, size_t* outLen) {
    switch (meta.validator) {
        case CFG_STRING:
        case CFG_IP:
        case CFG_PASSWORD: {
            if (!cJSON_IsString(item)) return false;
            size_t sl = strlen(item->valuestring);
            if (sl > 255 || cap < 1 + sl) return false;
            out[0] = static_cast<uint8_t>(sl);
            memcpy(out + 1, item->valuestring, sl);
            *outLen = 1 + sl;
            return true;
        }
        case CFG_FLOAT: {
            if (!cJSON_IsNumber(item)) return false;
            if (cap < sizeof(float)) return false;
            float f = static_cast<float>(item->valuedouble);
            memcpy(out, &f, sizeof(float));
            *outLen = sizeof(float);
            return true;
        }
        case CFG_INT: {
            if (!cJSON_IsNumber(item)) return false;
            int64_t v = static_cast<int64_t>(item->valuedouble);
            if (meta.size > sizeof(uint64_t) || cap < meta.size) return false;
            for (size_t i = 0; i < meta.size; ++i) {
                out[i] = static_cast<uint8_t>(static_cast<uint64_t>(v) >> (8 * i));
            }
            *outLen = meta.size;
            return true;
        }
        case CFG_UINT:
        case CFG_ENUM:
        case CFG_BOOL: {
            if (!cJSON_IsNumber(item)) return false;
            double d = item->valuedouble;
            if (d < 0) return false;
            uint64_t v = static_cast<uint64_t>(d);
            if (meta.size > sizeof(uint64_t) || cap < meta.size) return false;
            for (size_t i = 0; i < meta.size; ++i) {
                out[i] = static_cast<uint8_t>(v >> (8 * i));
            }
            *outLen = meta.size;
            return true;
        }
        default:
            return false;
    }
}

// Применить поля из JSON-объекта по именам (getMetaByName). Неизвестные
// ключи игнорируются — соответствующие поля остаются на дефолтах (решение #4).
// changed != nullptr — собирает UID реально изменившихся полей.
// hadUnknown != nullptr — устанавливается true при неизвестном ключе
// (устаревший формат config.json → автосейв перезапишет файл).
// trustedRestore=true — данные пришли с самого устройства (файл): runtime-поля
// восстанавливаются владельцем домена. false — извне (протокол): readonly-поля
// отклоняются (FieldWriteStatus::READONLY_DENIED).
static bool applyFieldsToCtx(AppContext* ctx, const cJSON* fields,
                             std::vector<uint16_t>* changed,
                             bool trustedRestore, bool* hadUnknown = nullptr) {
    if (!fields || !cJSON_IsObject(fields)) return false;

    AppDataLock dataLock(ctx);  // защита многополевой записи в adata

    bool anyApplied = false;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, fields) {
        if (!item->string) continue;

        const FieldMeta* meta = ctx->fields.getMetaByName(item->string);
        if (!meta) {
            if (hadUnknown) *hadUnknown = true;
            ESP_LOGW(TAG, "Unknown field in config JSON: %s", item->string);
            continue;
        }
        if (!meta->isConfig) continue;  // runtime-поля файлом не восстанавливаем

        uint8_t wire[kAppMaxFieldSize];
        size_t len = 0;
        if (!jsonToWireValue(*meta, item, wire, sizeof(wire), &len)) {
            ESP_LOGW(TAG, "Bad JSON value for field %s", meta->name);
            continue;
        }

        uint8_t oldBuf[kAppMaxFieldSize];
        size_t oldLen = 0;
        ctx->fields.readField(meta->uid, oldBuf, sizeof(oldBuf), &oldLen);

        FieldWriteStatus st =
            ctx->fields.writeField(meta->uid, wire, len,
                                   trustedRestore ? meta->domain
                                                  : FieldDomain::PROTOCOL);
        if (st != FieldWriteStatus::OK) {
            ESP_LOGW(TAG, "Field rejected on config apply: %s (status=%d)",
                     meta->name, static_cast<int>(st));
            continue;
        }
        anyApplied = true;

        if (changed) {
            uint8_t newBuf[kAppMaxFieldSize];
            size_t newLen = 0;
            ctx->fields.readField(meta->uid, newBuf, sizeof(newBuf), &newLen);
            if (oldLen != newLen || memcmp(oldBuf, newBuf, oldLen) != 0) {
                changed->push_back(meta->uid);
            }
        }
    }
    return anyApplied;
}

// ============================================================
// ConfigStore
// ============================================================

ConfigStore::ConfigStore(AppContext* ctx, const char* basePath,
                         const char* partitionLabel)
    : ctx_(ctx),
      basePath_(basePath ? basePath : "/config"),
      partitionLabel_(partitionLabel ? partitionLabel : "config"),
      fs_(basePath_.c_str(), partitionLabel_.c_str(), true) {}

ConfigStore::~ConfigStore() {
    // Отписка обязана быть и здесь: makeModule удаляет объект при ошибке
    // begin() — иначе обработчики CONFIG_CHANGED/FACTORY_RESET остались бы
    // на удалённый объект (UAF при первом же событии).
    if (ctx_)
        ctx_->events.unsubscribe(this);
    // Graceful stop автосейва — задачу нельзя убить посреди saveToFs
    // (запись в flash: open/write/rename). Паттерн DnsServer::stop.
    stopAutoSave();
}

esp_err_t ConfigStore::begin() {
    // NVS нужен драйверу Wi-Fi (конфиг WiFi-модуля), а также phy_init.
    // Инициализируем здесь — config первый модуль загрузки. При сбое не
    // фейлим модуль (он critical): сеть упадёт в degraded mode, а J1939-скан
    // продолжит работать.
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs = nvs_flash_init();
    }
    if (nvs != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed (%s) — WiFi will be down, degraded mode",
                 esp_err_to_name(nvs));
    }

    esp_err_t err = fs_.mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS: %s", esp_err_to_name(err));
        return err;
    }

    // Базовые дефолты до применения файла (AppData не имеет member-инициализаторов).
    initAppDataDefault(ctx_->adata);

    loadFromFs();

    // Семафор завершения — до создания задачи (паттерн DnsServer::start).
    stop_.store(false);
    doneSem_ = xSemaphoreCreateBinary();
    if (!doneSem_) {
        ESP_LOGE(TAG, "done semaphore create failed");
        return ESP_FAIL;
    }

    if (xTaskCreate(autoSaveWrapper, "cfg_autosave", 4096, this, 3, &task_) !=
        pdPASS) {
        ESP_LOGE(TAG, "Failed to create auto-save task");
        task_ = nullptr;
        vSemaphoreDelete(doneSem_);
        doneSem_ = nullptr;
        return ESP_FAIL;
    }

    // dirty_ ставится подписчиком CONFIG_CHANGED: автосейв сохранит JSON при
    // следующем тике. Также ловим FACTORY_RESET → сброс + рестарт.
    // Отказ подписки = отказ модуля (critical): dtor снимет частичные подписки
    // и удалит автосейв-задачу.
    if (!subscribeEvent<app_event_id_t::CONFIG_CHANGED>(
            ctx_->events, &ConfigStore::onConfigChanged, this) ||
        !subscribeEvent<app_event_id_t::FACTORY_RESET>(
            ctx_->events, &ConfigStore::reset, this)) {
        ESP_LOGE(TAG, "event subscribe failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ConfigStore started (littlefs: %s)",
             partitionLabel_.c_str());
    return ESP_OK;
}

void ConfigStore::onConfigChanged(const field_change_event_t* evt) {
    if (!evt) return;
    dirty_.store(true);
    ESP_LOGD(TAG, "field 0x%04X changed, marked for auto-save", evt->uid);
}

void ConfigStore::loadFromFs() {
    std::string full = basePath_ + kConfigPath;

    struct stat st;
    if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGW(TAG, "No config file '%s', using defaults (will save on first tick)",
                 full.c_str());
        dirty_.store(true);
        return;
    }
    if (st.st_size <= 0) {
        dirty_.store(true);
        return;
    }

    int fd = open(full.c_str(), O_RDONLY);
    if (fd < 0) {
        ESP_LOGW(TAG, "Failed to open config file '%s', using defaults",
                 full.c_str());
        dirty_.store(true);
        return;
    }

    // st_size+1 и явный NUL: cJSON_Parse читает строку, буфер ровно в размер
    // файла давал OOB-read за границей (мусор после данных ломал парсинг).
    // unique_ptr с deleter free: освобождение на всех путях выхода (RAII);
    // раньше — ручной free в 3 точках (пропуск = утечка).
    std::unique_ptr<char, decltype(&std::free)> buf(
        static_cast<char*>(std::malloc(static_cast<size_t>(st.st_size) + 1)),
        &std::free);
    if (!buf) {
        close(fd);
        dirty_.store(true);
        return;
    }
    ssize_t rd = read(fd, buf.get(), static_cast<size_t>(st.st_size));
    close(fd);
    if (rd != st.st_size) {
        dirty_.store(true);
        return;
    }
    buf.get()[rd] = '\0';

    cJSON* root = cJSON_Parse(buf.get());
    if (!root) {
        ESP_LOGE(TAG, "Config JSON parse failed, using defaults");
        dirty_.store(true);
        return;
    }

    // Применяем только известные поля (getMetaByName); неизвестные ключи
    // (например старый snake_case формат) игнорируются → остаются дефолты.
    // Если встретился хоть один неизвестный ключ — помечаем dirty_: автосейв
    // на следующем тике перезапишет файл в актуальном формате.
    bool hadUnknown = false;
    applyFieldsJson(root, true, &hadUnknown);
    cJSON_Delete(root);
    if (hadUnknown) {
        ESP_LOGW(TAG, "Legacy/unknown keys in config JSON, will rewrite in new format");
        dirty_.store(true);
    }
    ESP_LOGI(TAG, "Config loaded from %s", full.c_str());
}

// ============================================================
// JSON: adata → cJSON-объект (только isConfig-поля, ключ = имя поля)
// ============================================================

cJSON* ConfigStore::buildFieldsJson() {
    cJSON* obj = cJSON_CreateObject();
    if (!obj) return nullptr;

    AppDataLock dataLock(ctx_);
    for (size_t i = 0; i < kAppFieldCount; ++i) {
        const auto& meta = g_fieldMeta[i];
        if (!meta.isConfig) continue;

        const uint8_t* src =
            reinterpret_cast<const uint8_t*>(&ctx_->adata) + meta.offset;

        switch (meta.validator) {
            case CFG_STRING:
            case CFG_IP:
            case CFG_PASSWORD: {
                const FixedString* fs = reinterpret_cast<const FixedString*>(src);
                cJSON_AddStringToObject(obj, meta.name, fs->c_str());
                break;
            }
            case CFG_FLOAT: {
                float f;
                memcpy(&f, src, sizeof(float));
                cJSON_AddNumberToObject(obj, meta.name, static_cast<double>(f));
                break;
            }
            case CFG_INT: {
                int64_t v = readSignedLE(src, meta.size);
                cJSON_AddNumberToObject(obj, meta.name, static_cast<double>(v));
                break;
            }
            case CFG_UINT:
            case CFG_ENUM:
            case CFG_BOOL: {
                uint64_t v = readUnsignedLE(src, meta.size);
                cJSON_AddNumberToObject(obj, meta.name, static_cast<double>(v));
                break;
            }
            default:
                break;
        }
    }
    return obj;
}

// ============================================================
// Применение JSON-объекта к реестру
// ============================================================

// Восстановление конфига с устройства (trusted): readonly-поля пишутся их
// владельцем домена, чтобы персистed runtime-поля (если появятся) восстанавливались.
bool ConfigStore::applyFieldsJson(cJSON* root, bool trustedRestore,
                                  bool* hadUnknown) {
    if (!root) return false;
    return applyFieldsToCtx(ctx_, root, nullptr, trustedRestore, hadUnknown);
}

// Применить поля и уведомить систему (CONFIG_CHANGED + PUSH) только для
// реально изменившихся полей. Возвращает число изменённых полей.
// Вызывается из протокола (CommModule, STEP-03): запись извне — readonly-поля
// отклоняются (FieldDomain::PROTOCOL).
size_t ConfigStore::applyFieldsWithNotify(cJSON* fields) {
    if (!fields) return 0;

    std::vector<uint16_t> changed;
    applyFieldsToCtx(ctx_, fields, &changed, false);

    for (uint16_t uid : changed) {
        postFieldChanged(ctx_, uid);
        sendField(ctx_, uid);
    }
    return changed.size();
}

void ConfigStore::saveToFs() {
    // Сброс dirty_ ДО сборки JSON: изменение, пришедшее во время записи,
    // снова поставит флаг → автосейв повторится. Сброс после записи терял
    // бы такое изменение (гонка).
    dirty_.exchange(false);

    cJSON* root = buildFieldsJson();
    if (!root) {
        ESP_LOGE(TAG, "OOM creating config JSON");
        dirty_.store(true);   // dirty_ уже сброшен — вернуть, чтобы повторить
        return;
    }
    char* textRaw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!textRaw) {
        ESP_LOGE(TAG, "OOM printing config JSON");
        dirty_.store(true);
        return;
    }
    // unique_ptr с deleter cJSON_free: освобождение на всех путях выхода
    // (раньше — ручной cJSON_free в 3 точках, пропуск = утечка строки JSON).
    std::unique_ptr<char, decltype(&cJSON_free)> text(textRaw, &cJSON_free);

    // Атомарная запись: пишем tmp, проталкиваем в flash (fsync), затем rename —
    // LittleFS rename атомарен, при сбое питания конфиг не «уполовинивается».
    std::string full = basePath_ + kConfigPath;
    std::string tmp = full + ".tmp";

    int fd = open(tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (fd < 0) {
        ESP_LOGE(TAG, "Failed to open '%s' for write (errno=%d)",
                 tmp.c_str(), errno);
        dirty_.store(true);   // без этого изменение терялось бы навсегда
        return;
    }

    const char* p = text.get();
    size_t remaining = strlen(text.get());
    while (remaining > 0) {
        ssize_t w = write(fd, p, remaining);
        if (w < 0) {
            if (errno == EINTR) continue;
            ESP_LOGE(TAG, "Failed to write config '%s' (errno=%d)",
                     tmp.c_str(), errno);
            close(fd);
            dirty_.store(true);
            return;
        }
        p += w;
        remaining -= static_cast<size_t>(w);
    }
    fsync(fd);
    close(fd);

    if (rename(tmp.c_str(), full.c_str()) != 0) {
        ESP_LOGE(TAG, "Failed to rename '%s' -> '%s' (errno=%d)",
                 tmp.c_str(), full.c_str(), errno);
        dirty_.store(true);
        return;
    }

    ESP_LOGI(TAG, "Config saved to %s", full.c_str());
}

// ============================================================
// Factory reset
// ============================================================

esp_err_t ConfigStore::reset() {
    ESP_LOGW(TAG, "Factory reset: restoring defaults and clearing config");

    // Сначала гасим автосейв: иначе задача может ПИСАТЬ файл (dirty_ от
    // старых изменений) параллельно с unlink/сбросом — после unlink запись
    // «воскресит» config.json со старыми значениями (или убьёт reset посреди
    // манипуляций с FS). Задача даёт doneSem_ до удаления себя — join с
    // таймаутом; запись в flash укладывается в 2 с с запасом.
    stopAutoSave();

    {
        // Гонка с задачами WIFI/SYSTEM, пишущими adata: сброс дефолтов —
        // только под общим локом (рекурсивный, вложенность безопасна).
        AppDataLock dataLock(ctx_);
        initAppDataDefault(ctx_->adata);
    }

    // Удаляем файл конфига (+tmp), чтобы при старте применились дефолты.
    // Автосейв уже остановлен — никто не перезапишет файл после unlink.
    unlink((basePath_ + kConfigPath).c_str());
    unlink((basePath_ + kConfigPath + ".tmp").c_str());

    ESP_LOGW(TAG, "Restarting ESP32 after factory reset");
    esp_restart();   // не возвращает управление
    return ESP_OK;
}

void ConfigStore::autoSaveWrapper(void* p) {
    static_cast<ConfigStore*>(p)->autoSaveLoop();
}

void ConfigStore::autoSaveLoop() {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        // Проверка stop_ ПОСЛЕ задержки и ДО saveToFs: после запроса стопа
        // (dtor/reset) не начинать новую запись в flash; уже идущая saveToFs
        // завершается — join в stopAutoSave ждёт её (таймаут 2 с).
        if (stop_.load()) break;
        if (dirty_.load()) saveToFs();
    }
    // Give ДО сам удаления — stopAutoSave ждёт take; хэндл задачи дальше
    // трогать нельзя (только забыть).
    if (doneSem_) xSemaphoreGive(doneSem_);
    vTaskDelete(nullptr);
}

void ConfigStore::stopAutoSave() {
    // Ожидание нужно, только если задача создавалась (task_ + семафор есть).
    const bool needWait = (task_ != nullptr && doneSem_ != nullptr);
    stop_.store(true);
    if (needWait) {
        // Задача даёт doneSem_ ПЕРЕД vTaskDelete(nullptr); спит она не дольше
        // vTaskDelay(1000) плюс текущая saveToFs — таймаут 2000 мс покрывает.
        // Короткий повторный take сужает гонку «Give на границе таймаута».
        if (xSemaphoreTake(doneSem_, 2000 / portTICK_PERIOD_MS) == pdTRUE ||
            xSemaphoreTake(doneSem_, 100 / portTICK_PERIOD_MS) == pdTRUE) {
            task_ = nullptr;
        } else {
            // Задача зависла (Give не пришёл → она жива): принудительно
            // удаляем. После vTaskDelete удаление семафора ниже безопасно.
            ESP_LOGE(TAG, "autosave task did not stop in time, forcing delete");
            vTaskDelete(task_);
            task_ = nullptr;
        }
    }
    if (doneSem_) {
        vSemaphoreDelete(doneSem_);
        doneSem_ = nullptr;
    }
}