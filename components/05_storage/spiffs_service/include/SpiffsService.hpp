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