#include "ConfigStore.h"
#include "cJSON.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* TAG = "ConfigStore";

// Валидное числовое значение из JSON-узла в диапазоне [lo..hi],
// иначе — значение по умолчанию (дефолт из структуры AppConfig).
static uint32_t clampU32(const cJSON* item, uint32_t lo, uint32_t hi,
                         uint32_t def) {
    if (!cJSON_IsNumber(item)) return def;
    double v = item->valuedouble;
    if (v < static_cast<double>(lo) || v > static_cast<double>(hi)) return def;
    return static_cast<uint32_t>(v);
}

ConfigStore::ConfigStore(AppContext* ctx, const char* basePath,
                         const char* partitionLabel)
    : ctx_(ctx),
      basePath_(basePath ? basePath : "/config"),
      partitionLabel_(partitionLabel ? partitionLabel : "config"),
      fs_(basePath_.c_str(), partitionLabel_.c_str(), true) {}

ConfigStore::~ConfigStore() {
    if (task_) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
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

    loadFromFs();

    if (xTaskCreate(autoSaveWrapper, "cfg_autosave", 4096, this, 3, &task_) !=
        pdPASS) {
        ESP_LOGE(TAG, "Failed to create auto-save task");
        task_ = nullptr;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ConfigStore started (littlefs: %s)",
             partitionLabel_.c_str());
    return ESP_OK;
}

void ConfigStore::setAndSave(const AppConfig& next) {
    ctx_->config = next;
    dirty_ = true;

    // Задел под будущие команды протокола: уведомить подписчиков.
    config_changed_event_t evt = {0};
    ctx_->events.post(APP_EVENTS_BASE, app_event_id_t::CONFIG_CHANGED, evt);
}

void ConfigStore::loadFromFs() {
    std::string full = basePath_ + kConfigPath;

    struct stat st;
    if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGW(TAG, "No config file '%s', using defaults (will save on first tick)",
                 full.c_str());
        dirty_ = true;
        return;
    }
    if (st.st_size <= 0) {
        dirty_ = true;
        return;
    }

    int fd = open(full.c_str(), O_RDONLY);
    if (fd < 0) {
        ESP_LOGW(TAG, "Failed to open config file '%s', using defaults",
                 full.c_str());
        dirty_ = true;
        return;
    }

    char* buf = static_cast<char*>(malloc(static_cast<size_t>(st.st_size)));
    if (!buf) {
        close(fd);
        dirty_ = true;
        return;
    }
    ssize_t rd = read(fd, buf, static_cast<size_t>(st.st_size));
    close(fd);
    if (rd != st.st_size) {
        free(buf);
        dirty_ = true;
        return;
    }

    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "Config JSON parse failed, using defaults");
        dirty_ = true;
        return;
    }

    // Строковые поля: только валидные, в буфер структуры не больше размера.
    const cJSON* ssid = cJSON_GetObjectItem(root, "ap_ssid");
    if (cJSON_IsString(ssid) && ssid->valuestring && ssid->valuestring[0]) {
        snprintf(ctx_->config.apSsid, sizeof ctx_->config.apSsid, "%s",
                 ssid->valuestring);
    }
    const cJSON* pass = cJSON_GetObjectItem(root, "ap_password");
    if (cJSON_IsString(pass) && pass->valuestring && pass->valuestring[0]) {
        snprintf(ctx_->config.apPassword, sizeof ctx_->config.apPassword, "%s",
                 pass->valuestring);
    }

    // Числовые поля с диапазонами (невалидные → дефолт структуры).
    const cJSON* it = cJSON_GetObjectItem(root, "ap_channel");
    // Страна по умолчанию (cc=01) разрешает каналы 1..11; 12+ уронит
    // esp_wifi_set_config — поэтому валидируем строго по этому диапазону.
    ctx_->config.apChannel =
        static_cast<uint8_t>(clampU32(it, 1, 11, ctx_->config.apChannel));

    it = cJSON_GetObjectItem(root, "max_sta_conn");
    ctx_->config.maxStaConn =
        static_cast<uint8_t>(clampU32(it, 1, 8, ctx_->config.maxStaConn));

    it = cJSON_GetObjectItem(root, "snapshot_interval_ms");
    ctx_->config.snapshotIntervalMs =
        clampU32(it, 100, 1000, ctx_->config.snapshotIntervalMs);

    it = cJSON_GetObjectItem(root, "snapshot_ttl_ms");
    ctx_->config.snapshotTtlMs =
        clampU32(it, 500, 10000, ctx_->config.snapshotTtlMs);

    it = cJSON_GetObjectItem(root, "max_tracked_pgns");
    ctx_->config.maxTrackedPgns = static_cast<uint16_t>(
        clampU32(it, 16, 256, ctx_->config.maxTrackedPgns));

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Config loaded from %s", full.c_str());
}

void ConfigStore::saveToFs() {
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "OOM creating config JSON");
        return;
    }
    cJSON_AddStringToObject(root, "ap_ssid", ctx_->config.apSsid);
    cJSON_AddStringToObject(root, "ap_password", ctx_->config.apPassword);
    cJSON_AddNumberToObject(root, "ap_channel", ctx_->config.apChannel);
    cJSON_AddNumberToObject(root, "max_sta_conn", ctx_->config.maxStaConn);
    cJSON_AddNumberToObject(root, "snapshot_interval_ms",
                            static_cast<double>(ctx_->config.snapshotIntervalMs));
    cJSON_AddNumberToObject(root, "snapshot_ttl_ms",
                            static_cast<double>(ctx_->config.snapshotTtlMs));
    cJSON_AddNumberToObject(root, "max_tracked_pgns",
                            static_cast<double>(ctx_->config.maxTrackedPgns));

    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        ESP_LOGE(TAG, "OOM printing config JSON");
        return;
    }

    // Атомарная запись: пишем tmp, проталкиваем в flash (fsync), затем rename —
    // LittleFS rename атомарен, при сбое питания конфиг не «уполовинивается».
    std::string full = basePath_ + kConfigPath;
    std::string tmp = full + ".tmp";

    int fd = open(tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (fd < 0) {
        ESP_LOGE(TAG, "Failed to open '%s' for write (errno=%d)",
                 tmp.c_str(), errno);
        cJSON_free(text);
        return;
    }

    const char* p = text;
    size_t remaining = strlen(text);
    while (remaining > 0) {
        ssize_t w = write(fd, p, remaining);
        if (w < 0) {
            if (errno == EINTR) continue;
            ESP_LOGE(TAG, "Failed to write config '%s' (errno=%d)",
                     tmp.c_str(), errno);
            close(fd);
            cJSON_free(text);
            return;
        }
        p += w;
        remaining -= static_cast<size_t>(w);
    }
    fsync(fd);
    close(fd);
    cJSON_free(text);

    if (rename(tmp.c_str(), full.c_str()) != 0) {
        ESP_LOGE(TAG, "Failed to rename '%s' -> '%s' (errno=%d)",
                 tmp.c_str(), full.c_str(), errno);
        return;
    }

    dirty_ = false;
    ESP_LOGI(TAG, "Config saved to %s", full.c_str());
}

void ConfigStore::autoSaveWrapper(void* p) {
    static_cast<ConfigStore*>(p)->autoSaveLoop();
}

void ConfigStore::autoSaveLoop() {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (dirty_) saveToFs();
    }
}