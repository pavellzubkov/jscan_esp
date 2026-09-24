#include "BootManager.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "BootManager";

esp_err_t BootManager::add(const Entry& entry) {
    if (count_ >= kMaxModules) {
        ESP_LOGE(TAG, "Cannot add [%s] — limit %zu reached", entry.name, kMaxModules);
        return ESP_ERR_NO_MEM;
    }
    entries_[count_++] = entry;
    ESP_LOGD(TAG, "Registered [%s] priority=%u critical=%s",
             entry.name, entry.priority, entry.critical ? "YES" : "no");
    return ESP_OK;
}

// =======================
// Сортировка вставками — для малых N быстрее qsort
// =======================
void BootManager::sortByPriority(Entry* entries, size_t count) {
    for (size_t i = 1; i < count; i++) {
        Entry key = entries[i];
        int j = static_cast<int>(i) - 1;
        while (j >= 0 && entries[j].priority > key.priority) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = key;
    }
}

// =======================
// Запуск всех модулей
// =======================
esp_err_t BootManager::startAll(AppContext* ctx) {
    sortByPriority(entries_, count_);

    ESP_LOGI(TAG, "========== Starting %zu modules ==========", count_);
    esp_err_t firstError = ESP_OK;

    for (size_t i = 0; i < count_; i++) {
        ESP_LOGI(TAG, "[%s] starting... (priority=%u, critical=%s)",
                 entries_[i].name,
                 entries_[i].priority,
                 entries_[i].critical ? "YES" : "no");

        esp_err_t err = entries_[i].init(ctx);
        results_[i] = err;
        started_[i] = true;

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[%s] FAILED: %s", entries_[i].name, esp_err_to_name(err));
            if (firstError == ESP_OK) {
                firstError = err;
            }
            if (entries_[i].critical) {
                ESP_LOGE(TAG, "[%s] is CRITICAL — aborting startup", entries_[i].name);
                return err;
            }
        } else {
            ESP_LOGI(TAG, "[%s] OK", entries_[i].name);
        }
    }

    dumpStatus();
    return firstError;
}

// =======================
// Статус
// =======================
void BootManager::dumpStatus() const {
    ESP_LOGI(TAG, "========== Module Status ==========");
    size_t ok = 0, fail = 0;
    for (size_t i = 0; i < count_; i++) {
        if (!started_[i]) {
            ESP_LOGW(TAG, "  [SKIP] %s", entries_[i].name);
            continue;
        }
        if (results_[i] == ESP_OK) {
            ESP_LOGI(TAG, "  [ OK ] %s", entries_[i].name);
            ok++;
        } else {
            ESP_LOGE(TAG, "  [FAIL] %s : %s",
                     entries_[i].name, esp_err_to_name(results_[i]));
            fail++;
        }
    }
    ESP_LOGI(TAG, "===================================");
    ESP_LOGI(TAG, "Total: %zu | OK: %zu | FAIL: %zu", count_, ok, fail);
}

bool BootManager::isReady(const char* name) const {
    for (size_t i = 0; i < count_; i++) {
        if (std::strcmp(entries_[i].name, name) == 0) {
            return started_[i] && results_[i] == ESP_OK;
        }
    }
    return false;
}
