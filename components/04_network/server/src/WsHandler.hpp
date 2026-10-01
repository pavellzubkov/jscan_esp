#pragma once
#include "esp_http_server.h"
#include "AppContext.h"

class WsHandler {
public:
    explicit WsHandler(AppContext* ctx);
    ~WsHandler();   // снять подписку WS_MESSAGE_SEND (сделана в reg)

    esp_err_t reg(httpd_handle_t server);
    void unreg();

private:
    // Потолок подключений WS: один источник для массива, проверки в
    // add_client и локальной копии в send_to_all_clients.
    static constexpr int kMaxClients = 10;

    AppContext* ctx_;
    httpd_handle_t server_ = nullptr;
    int connected_clients_[kMaxClients];
    int client_count_ = 0;
    // Подписка на WS_MESSAGE_SEND выполняется один раз за время жизни объекта
    // (reg() может вызываться многократно — рестарт httpd в NetworkController).
    bool subs_registered_ = false;

    // Подписка на app_event_id_t::WS_MESSAGE_SEND (через EventManager)
    void onWsMessageSend(const ws_message_t* msg);

    // HTTP/WS handler (статический — вызывается httpd-сервером)
    static esp_err_t ws_handler(httpd_req_t* req);

    // WS post-handshake callback (IDF >= 5.5: HTTP GET handler больше не вызывается,
    // поэтому клиент регистрируем здесь, после завершения хэндшейка)
    static esp_err_t onPostHandshake(httpd_req_t* req);

    void add_client(int sockfd);
    void remove_client(int sockfd);
    void cleanup_clients();
    void send_to_all_clients(const char* data, size_t len);
};