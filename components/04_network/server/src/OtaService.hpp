#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "AppContext.h"
#include "LittleFsService.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

class OtaService {
public:
    enum class State {
        IDLE,          // готово принимать storage
        WRITING_STORAGE,
        STORAGE_DONE,  // storage записан и смонтирован, ждём app
        WRITING_APP,
        REBOOTING,     // образ app записан, бут-партиция выбрана, ждём рестарт
        ERROR,         // ошибка; следующая попытка сбросит в IDLE
    };

    OtaService() = default;

    void init(AppContext* ctx, LittleFsService* fs);

    // POST /api/ota/storage
    esp_err_t handleStorageUpload(httpd_req_t* req);
    // POST /api/ota/app
    esp_err_t handleAppUpload(httpd_req_t* req);

    // Заполняет cJSON объект текущим статусом (вызывается из GET /api/ota/status)
    void fillStatusJson(void* root);

private:
    static esp_err_t recvToCallback(httpd_req_t* req, size_t expected,
                                    bool (*cb)(void* arg, const uint8_t* data, size_t len),
                                    void* cb_arg, std::atomic<size_t>* received);

    // Перевести систему в безопасное состояние перед flash-операциями
    // (событие OTA_BEGIN). В jscan термоконтура нет — только уведомление.
    void enterOta();
    // Вернуть управление после flash-операций (событие OTA_END).
    void exitOta();

    AppContext* ctx_ = nullptr;
    LittleFsService* fs_ = nullptr;

    std::mutex mux_;
    State state_ = State::IDLE;
    std::atomic<size_t> received_{0};  // принято байт в текущем шаге
    std::atomic<size_t> expected_{0};  // ожидается байт в текущем шаге
    std::string error_;                // текст ошибки (для ERROR)

    static constexpr size_t kBufSize = 8192;
};