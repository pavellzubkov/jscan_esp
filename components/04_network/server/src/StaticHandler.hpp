#pragma once
#include "esp_http_server.h"
#include "esp_vfs.h"
#include <cstddef>

// Контекст wildcard-хендлера статики: базовый путь ФС + scratch-буфер
// потоковой передачи (~4 КБ). Владелец — ServerModule: аллокация в
// reg_static_handler (out-параметр), освобождение в ServerModule::stop()
// строго ПОСЛЕ httpd_stop (иначе UAF в httpd-задаче).
struct static_ctx_t {
    static constexpr size_t kScratchSize = 4096;
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[kScratchSize];
};

// Регистрирует глобальный GET /* для раздачи статики из LittleFS
// (index.html для '/', gzip .gz fallback, Access-Control-Allow-Origin: *).
// При успехе записывает аллоцированный контекст в *out_ctx (nullptr — при
// ошибке); контекст не освобождается здесь — владеет вызывающий.
esp_err_t reg_static_handler(httpd_handle_t server, static_ctx_t** out_ctx);