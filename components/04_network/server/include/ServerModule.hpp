#pragma once
#include "esp_http_server.h"
#include "AppContext.h"
#include "LittleFsService.hpp"

class WsHandler;   // fwd, чтобы не тянуть WsHandler.hpp в include/

class ServerModule {
public:
    explicit ServerModule(AppContext* ctx);
    ~ServerModule();

    esp_err_t begin();   // смонтировать ФС + старт httpd + static + ws
    void      stop();

    httpd_handle_t getHandle() const { return server_; }

private:
    AppContext* ctx_;
    httpd_handle_t server_ = nullptr;
    WsHandler* ws_ = nullptr;
    LittleFsService fs_;   // /littlefs, "storage"; статика фронта
};