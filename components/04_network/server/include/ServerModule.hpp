#pragma once
#include "esp_http_server.h"
#include "AppContext.h"
#include "LittleFsService.hpp"
#include "../src/OtaService.hpp"

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
    WsHandler* ws_ = nullptr;
    static_ctx_t* staticCtx_ = nullptr;   // ~4 КБ; delete только после httpd_stop
    LittleFsService fs_;   // /littlefs, "storage"; статика фронта
    OtaService ota_;
};