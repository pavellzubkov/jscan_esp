#pragma once
#include "AppContext.hpp"
#include "WifiApModule.hpp"
#include "ServerModule.hpp"

// Координатор сети: владеет Wi-Fi (AP) и веб-сервером (HTTP+WS).
class NetworkController {
public:
    explicit NetworkController(AppContext* ctx);
    ~NetworkController();

    esp_err_t begin();   // wifi.begin() → server.begin()
    void      stop();    // server.stop() → wifi.stop()

private:
    AppContext* ctx_;
    WifiApModule wifi_;
    ServerModule server_;
};
