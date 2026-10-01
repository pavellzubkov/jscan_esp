#pragma once
#include "esp_http_server.h"
#include "AppContext.h"
#include "LittleFsService.hpp"
#include "../src/OtaService.hpp"
#include <memory>

class WsHandler;   // fwd, чтобы не тянуть WsHandler.hpp в include/
struct static_ctx_t;   // контекст wildcard-хендлера статики (StaticHandler.hpp)

class ServerModule {
public:
    explicit ServerModule(AppContext* ctx);
    ~ServerModule();

    esp_err_t begin();   // смонтировать ФС + старт httpd + static + ws + ota
    void      stop();

    httpd_handle_t getHandle() const { return server_; }

private:
    AppContext* ctx_;
    httpd_handle_t server_ = nullptr;
    // unique_ptr: владение с авто-удалением. Порядок освобождения задаёт
    // stop() (httpd_stop → ws->unreg → reset → staticCtx_.reset строго
    // после httpd_stop — иначе UAF в httpd-задаче; см. ServerModule.cpp).
    // ~ServerModule объявлен здесь, определён в .cpp, где WsHandler/
    // static_ctx_t — полные типы → удаление безопасно.
    std::unique_ptr<WsHandler> ws_;
    std::unique_ptr<static_ctx_t> staticCtx_;   // ~4 КБ; освобождается после httpd_stop
    LittleFsService fs_;   // /littlefs, "storage"; статика фронта
    OtaService ota_;
};