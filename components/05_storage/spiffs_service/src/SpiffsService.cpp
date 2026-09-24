#include "SpiffsService.hpp"
#include "esp_log.h"
#include "esp_spiffs.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>

static const char* TAG = "SpiffsService";

SpiffsService::SpiffsService(const char* basePath, const char* partitionLabel,
                             bool formatOnFail)
    : formatOnFail_(formatOnFail) {
    snprintf(basePath_, sizeof basePath_, "%s",
             basePath ? basePath : "/spiffs");
    snprintf(partitionLabel_, sizeof partitionLabel_, "%s",
             partitionLabel ? partitionLabel : "storage");
}

SpiffsService::~SpiffsService() {
    unmount();
}

esp_err_t SpiffsService::mount() {
    if (mounted_) return ESP_OK;

    // Идемпотентность: если ФС уже зарегистрирована (например, другим модулем),
    // повторный mount не нужен.
    if (esp_spiffs_mounted(partitionLabel_)) {
        mounted_ = true;
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = basePath_,
        .partition_label = partitionLabel_,
        .max_files = 5,
        .format_if_mount_failed = formatOnFail_,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem on '%s'",
                     partitionLabel_);
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition '%s'",
                     partitionLabel_);
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS '%s' (%s)",
                     partitionLabel_, esp_err_to_name(ret));
        }
        return ret;
    }
    mounted_ = true;

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(partitionLabel_, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Partition '%s' size: total: %d, used: %d",
                 partitionLabel_, (int)total, (int)used);
    } else {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)",
                 esp_err_to_name(ret));
    }
    return ESP_OK;
}

void SpiffsService::unmount() {
    if (!mounted_) return;
    esp_vfs_spiffs_unregister(partitionLabel_);
    mounted_ = false;
}

bool SpiffsService::fileExists(const char* relPath) const {
    if (!relPath) return false;
    char full[192];
    snprintf(full, sizeof full, "%s/%s", basePath_, relPath);
    return access(full, F_OK) == 0;
}

esp_err_t SpiffsService::readFile(const char* relPath, char** outBuf,
                                  size_t* outLen) const {
    if (!relPath || !outBuf || !outLen) return ESP_ERR_INVALID_ARG;
    *outBuf = nullptr;
    *outLen = 0;

    char full[192];
    snprintf(full, sizeof full, "%s/%s", basePath_, relPath);
    FILE* f = fopen(full, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return ESP_FAIL;
    }

    char* buf = static_cast<char*>(malloc(size > 0 ? static_cast<size_t>(size)
                                                   : 1u));
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t rd = fread(buf, 1, static_cast<size_t>(size), f);
    fclose(f);

    *outBuf = buf;
    *outLen = rd;
    return ESP_OK;
}

esp_err_t SpiffsService::writeFile(const char* relPath, const void* data,
                                   size_t len) const {
    if (!relPath || !data) return ESP_ERR_INVALID_ARG;
    char full[192];
    snprintf(full, sizeof full, "%s/%s", basePath_, relPath);
    FILE* f = fopen(full, "wb");
    if (!f) return ESP_FAIL;
    size_t wr = fwrite(data, 1, len, f);
    fclose(f);
    return wr == len ? ESP_OK : ESP_FAIL;
}

esp_err_t SpiffsService::removeFile(const char* relPath) const {
    if (!relPath) return ESP_ERR_INVALID_ARG;
    char full[192];
    snprintf(full, sizeof full, "%s/%s", basePath_, relPath);
    return unlink(full) == 0 ? ESP_OK : ESP_FAIL;
}