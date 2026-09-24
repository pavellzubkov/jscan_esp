#include "ConfigStore.h"
#include "cJSON.h"
#include "esp_log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

ConfigStore::ConfigStore(AppContext* ctx)
    : ctx_(ctx),
      fs_("/spiffs", "storage", true) {}

ConfigStore::~ConfigStore() {
    if (task_) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
}

esp_err_t ConfigStore::begin() {
    esp_err_t err = fs_.mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS: %s", esp_err_to_name(err));
        return err;
    }

    loadFromFs();

    if (xTaskCreate(autoSaveWrapper, "cfg_autosave", 4096, this, 3, &task_) !=
        pdPASS) {
        ESP_LOGE(TAG, "Failed to create auto-save task");
        task_ = nullptr;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ConfigStore started");
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
    char* buf = nullptr;
    size_t len = 0;
    if (fs_.readFile(kConfigPath, &buf, &len) != ESP_OK) {
        if (buf) free(buf);
        ESP_LOGW(TAG, "No config file '%s', using defaults (will save on first tick)",
                 kConfigPath);
        dirty_ = true;
        return;
    }
    if (len == 0 || !buf) {
        if (buf) free(buf);
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
    ctx_->config.apChannel =
        static_cast<uint8_t>(clampU32(it, 1, 14, ctx_->config.apChannel));

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
    ESP_LOGI(TAG, "Config loaded from %s", kConfigPath);
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

    esp_err_t err = fs_.writeFile(kConfigPath, text, strlen(text));
    cJSON_free(text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save config '%s': %s", kConfigPath,
                 esp_err_to_name(err));
        return;
    }
    dirty_ = false;
    ESP_LOGI(TAG, "Config saved to %s", kConfigPath);
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