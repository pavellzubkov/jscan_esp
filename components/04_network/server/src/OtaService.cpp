#include "OtaService.hpp"
#include "HttpCommon.hpp"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdio>
#include <cstring>

static const char* TAG = "OtaService";

namespace {

// Штатный таймаут Task WDT (из Kconfig).
constexpr uint32_t kNormalWdtTimeoutMs = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000;
// На время OTA таймаут расширяется: erase/write флеша отключает кэш на обоих
// ядрах, поэтому задачи физически не могут кормить WDT.
constexpr uint32_t kOtaWdtTimeoutMs = 120000;
// Маска idle-ядер, которые нужно снова подписать при восстановлении.
constexpr uint32_t kIdleCoreMask =
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
    (1u << 0) |
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
    (1u << 1) |
#endif
    0u;

// RAII: расширяет таймаут Task WDT на время flash-операций и возвращает
// штатный при выходе из области видимости.
class WdtPause {
public:
    WdtPause() {
        esp_task_wdt_config_t cfg = { kOtaWdtTimeoutMs, 0u, true };
        if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) {
            ESP_LOGW(TAG, "task WDT pause failed, OTA may trip watchdog");
        }
    }
    ~WdtPause() {
        esp_task_wdt_config_t cfg = { kNormalWdtTimeoutMs, kIdleCoreMask, true };
        if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) {
            ESP_LOGW(TAG, "task WDT resume failed");
        }
    }
    WdtPause(const WdtPause&) = delete;
    WdtPause& operator=(const WdtPause&) = delete;
};

} // namespace

static const char* stateToString(OtaService::State s) {
    switch (s) {
    case OtaService::State::IDLE:           return "idle";
    case OtaService::State::WRITING_STORAGE:return "writing_storage";
    case OtaService::State::STORAGE_DONE:   return "storage_done";
    case OtaService::State::WRITING_APP:    return "writing_app";
    case OtaService::State::REBOOTING:      return "rebooting";
    case OtaService::State::ERROR:          return "error";
    }
    return "unknown";
}

void OtaService::init(AppContext* ctx, LittleFsService* fs) {
    ctx_ = ctx;
    fs_ = fs;
}

void OtaService::enterOta() {
    if (!ctx_) return;
    ctx_->events.post(APP_EVENTS_BASE, app_event_id_t::OTA_BEGIN);
}

void OtaService::exitOta() {
    if (!ctx_) return;
    ctx_->events.post(APP_EVENTS_BASE, app_event_id_t::OTA_END);
}

// ---------------------------------------------------------------
// Потоковый приём тела запроса: читает ровно expected байт и кормит
// ими callback. Таймаут между сегментами TCP не считается ошибкой —
// при медленном клиенте просто ждём дальше (recv_wait_timeout = 1 c).
// ---------------------------------------------------------------
esp_err_t OtaService::recvToCallback(httpd_req_t* req, size_t expected,
                                     bool (*cb)(void* arg, const uint8_t* data, size_t len),
                                     void* cb_arg, std::atomic<size_t>* received) {
    // Буфер на куче, а не на стеке: задача httpd имеет стек 16384 байт, но
    // kBufSize=8192 + остальные фреймы — лучше не рисковать стеком.
    char* buf = (char*)malloc(kBufSize);
    if (!buf) {
        ESP_LOGE(TAG, "OOM: cannot allocate recv buffer");
        return ESP_ERR_NO_MEM;
    }
    size_t remaining = expected;

    while (remaining > 0) {
        int len = httpd_req_recv(req, buf, remaining < kBufSize ? remaining : kBufSize);
        if (len < 0) {
            if (len == HTTPD_SOCK_ERR_TIMEOUT) {
                continue; // медленный клиент — ждём следующий фрагмент
            }
            ESP_LOGE(TAG, "recv error: %d", len);
            free(buf);
            return ESP_FAIL;
        }
        if (len == 0) {
            ESP_LOGE(TAG, "connection closed by peer");
            free(buf);
            return ESP_FAIL;
        }
        if (!cb(cb_arg, reinterpret_cast<const uint8_t*>(buf), (size_t)len)) {
            ESP_LOGE(TAG, "write callback failed");
            free(buf);
            return ESP_FAIL;
        }
        *received += (size_t)len;
        remaining -= (size_t)len;
    }
    free(buf);
    return ESP_OK;
}

// ---------------------------------------------------------------
// POST /api/ota/storage
// ---------------------------------------------------------------
esp_err_t OtaService::handleStorageUpload(httpd_req_t* req) {
    http::setJsonHeaders(req);

    {
        std::lock_guard<std::mutex> lock(mux_);
        if (state_ == State::ERROR) {
            state_ = State::IDLE; // повторная попытка после ошибки
        } else if (state_ != State::IDLE) {
            http::setStatus(req, http::status::kConflict);
            return httpd_resp_sendstr(req, R"({"error":"ota busy"})");
        }
        expected_ = (size_t)req->content_len;
        received_ = 0;
        error_.clear();
        state_ = State::WRITING_STORAGE;
    }

    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "storage");
    if (!part) {
        {
            std::lock_guard<std::mutex> lock(mux_);
            state_ = State::ERROR;
            error_ = "storage partition not found";
        }
        ESP_LOGE(TAG, "storage partition not found");
        http::setStatus(req, http::status::kInternalServerError);
        return httpd_resp_sendstr(req, R"({"error":"no storage partition"})");
    }

    size_t partSize = part->size;
    if (req->content_len != (int)partSize) {
        {
            std::lock_guard<std::mutex> lock(mux_);
            state_ = State::ERROR;
            error_ = "size mismatch";
        }
        char buf[96];
        snprintf(buf, sizeof(buf),
                 R"({"error":"expected %u bytes, got %d"})", partSize, req->content_len);
        http::setStatus(req, http::status::kBadRequest);
        return httpd_resp_sendstr(req, buf);
    }

    // Task WDT расширяется, т.к. erase/write блокируют выполнение задач,
    // кормящих watchdog. OTA_END публикуется на любом пути выхода из функции.
    WdtPause wdtPause;
    enterOta();
    struct OtaEndGuard {
        OtaService* self;
        ~OtaEndGuard() { self->exitOta(); }
    } otaEndGuard{ this };

    // Файловая система размонтируется, чтобы не модифицировать образ во время записи.
    esp_err_t err = fs_->unmount();
    if (err != ESP_OK) {
        {
            std::lock_guard<std::mutex> lock(mux_);
            state_ = State::ERROR;
            error_ = "unmount failed";
        }
        ESP_LOGE(TAG, "failed to unmount LittleFS: %s", esp_err_to_name(err));
        http::setStatus(req, http::status::kInternalServerError);
        return httpd_resp_sendstr(req, R"({"error":"unmount failed"})");
    }

    err = esp_partition_erase_range(part, 0, partSize);
    if (err != ESP_OK) {
        {
            std::lock_guard<std::mutex> lock(mux_);
            state_ = State::ERROR;
            error_ = "erase failed";
        }
        ESP_LOGE(TAG, "failed to erase storage partition: %s", esp_err_to_name(err));
        http::setStatus(req, http::status::kInternalServerError);
        return httpd_resp_sendstr(req, R"({"error":"erase failed"})");
    }

    struct WriteCtx {
        const esp_partition_t* part;
        size_t off;
    } wctx = { part, 0 };

    auto writeCb = [](void* arg, const uint8_t* data, size_t len) -> bool {
        auto* c = static_cast<WriteCtx*>(arg);
        if (esp_partition_write(c->part, c->off, data, len) != ESP_OK) {
            return false;
        }
        c->off += len;
        return true;
    };

    bool uploadOk = (recvToCallback(req, partSize, writeCb, &wctx, &received_) == ESP_OK);

    // Монтируем БЕЗ автоформата: после записи образа нельзя молча стирать раздел
    // и рапортовать успех. Если образ/запись повреждены — вернём ошибку,
    // а не потеряем данные фронтенда.
    err = fs_->mount(false);
    if (err != ESP_OK) {
        {
            std::lock_guard<std::mutex> lock(mux_);
            state_ = State::ERROR;
            error_ = uploadOk ? "storage image invalid" : "fs remount failed";
        }
        ESP_LOGE(TAG, "failed to remount LittleFS after write: %s", esp_err_to_name(err));
        http::setStatus(req, http::status::kInternalServerError);
        return httpd_resp_sendstr(req, R"({"error":"fs remount failed"})");
    }

    if (!uploadOk) {
        {
            std::lock_guard<std::mutex> lock(mux_);
            state_ = State::ERROR;
            error_ = "upload interrupted";
        }
        http::setStatus(req, http::status::kInternalServerError);
        return httpd_resp_sendstr(req, R"({"error":"upload interrupted"})");
    }

    {
        std::lock_guard<std::mutex> lock(mux_);
        state_ = State::STORAGE_DONE;
        error_.clear();
    }
    ESP_LOGI(TAG, "storage.bin written and remounted (%u bytes)", (unsigned)partSize);
    return httpd_resp_sendstr(req, R"({"status":"storage_done"})");
}

// ---------------------------------------------------------------
// POST /api/ota/app
// ---------------------------------------------------------------
esp_err_t OtaService::handleAppUpload(httpd_req_t* req) {
    http::setJsonHeaders(req);

    {
        std::lock_guard<std::mutex> lock(mux_);
        if (state_ != State::STORAGE_DONE) {
            http::setStatus(req, http::status::kConflict);
            return httpd_resp_sendstr(req,
                R"({"error":"storage must be uploaded first"})");
        }
        expected_ = (size_t)req->content_len;
        received_ = 0;
        error_.clear();
        state_ = State::WRITING_APP;
    }

    // Как и для storage: расширить Task WDT на время записи образа. При успехе
    // esp_restart() не вернёт управление — guard'ы не выполнятся, но это не важно
    // (устройство перезагружается).
    WdtPause wdtPause;
    enterOta();
    struct OtaEndGuard {
        OtaService* self;
        ~OtaEndGuard() { self->exitOta(); }
    } otaEndGuard{ this };

    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part) {
        {
            std::lock_guard<std::mutex> lock(mux_);
            state_ = State::ERROR;
            error_ = "no ota partition";
        }
        ESP_LOGE(TAG, "no OTA update partition available");
        http::setStatus(req, http::status::kInternalServerError);
        return httpd_resp_sendstr(req, R"({"error":"no ota partition"})");
    }

    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        {
            std::lock_guard<std::mutex> lock(mux_);
            state_ = State::ERROR;
            error_ = "ota begin failed";
        }
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        http::setStatus(req, http::status::kInternalServerError);
        return httpd_resp_sendstr(req, R"({"error":"ota begin failed"})");
    }

    struct WriteCtx {
        esp_ota_handle_t handle;
    } wctx = { handle };

    auto writeCb = [](void* arg, const uint8_t* data, size_t len) -> bool {
        auto* c = static_cast<WriteCtx*>(arg);
        return esp_ota_write(c->handle, data, len) == ESP_OK;
    };

    bool uploadOk = (recvToCallback(req, (size_t)req->content_len, writeCb,
                                    &wctx, &received_) == ESP_OK);

    if (!uploadOk) {
        esp_ota_abort(handle);
        {
            std::lock_guard<std::mutex> lock(mux_);
            state_ = State::ERROR;
            error_ = "upload interrupted";
        }
        ESP_LOGE(TAG, "app upload interrupted, aborted");
        http::setStatus(req, http::status::kInternalServerError);
        return httpd_resp_sendstr(req, R"({"error":"upload interrupted"})");
    }

    // esp_ota_end проверяет целостность образа (SHA/MD5 заголовка)
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        {
            std::lock_guard<std::mutex> lock(mux_);
            state_ = State::ERROR;
            error_ = "image invalid";
        }
        ESP_LOGE(TAG, "esp_ota_end failed (image invalid): %s", esp_err_to_name(err));
        http::setStatus(req, http::status::kBadRequest);
        return httpd_resp_sendstr(req, R"({"error":"image invalid"})");
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        {
            std::lock_guard<std::mutex> lock(mux_);
            state_ = State::ERROR;
            error_ = "set boot failed";
        }
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        http::setStatus(req, http::status::kInternalServerError);
        return httpd_resp_sendstr(req, R"({"error":"set boot failed"})");
    }

    {
        std::lock_guard<std::mutex> lock(mux_);
        state_ = State::REBOOTING;
    }
    ESP_LOGI(TAG, "app written to %s, rebooting...", part->label);
    httpd_resp_sendstr(req, R"({"status":"rebooting"})");

    // Даём TCP-стекам время уйти от клиента, затем перезагрузка.
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK; // не достижимо
}

// ---------------------------------------------------------------
// GET /api/ota/status — заполняет cJSON (root уже создан)
// ---------------------------------------------------------------
void OtaService::fillStatusJson(void* root) {
    auto* j = static_cast<cJSON*>(root);

    State s;
    std::string err;
    {
        std::lock_guard<std::mutex> lock(mux_);
        s = state_;
        err = error_;
    }

    cJSON_AddStringToObject(j, "status", stateToString(s));
    cJSON_AddNumberToObject(j, "received", (double)received_.load());
    cJSON_AddNumberToObject(j, "expected", (double)expected_.load());
    if (s == State::ERROR && !err.empty()) {
        cJSON_AddStringToObject(j, "error", err.c_str());
    }
}