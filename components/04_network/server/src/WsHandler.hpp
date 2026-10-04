#pragma once
#include "esp_http_server.h"
#include "AppContext.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <atomic>
#include <mutex>

class WsHandler {
public:
    explicit WsHandler(AppContext* ctx);
    ~WsHandler();   // остановить sender, снять подписку WS_MESSAGE_SEND

    esp_err_t reg(httpd_handle_t server);
    void unreg();
    // Остановить sender-задачу (идемпотентно). Вызывается ДО httpd_stop:
    // задача не должна слать кадры в останавливаемый сервер.
    void stopTx();

private:
    // Потолок подключений WS: один источник для массива, проверки в
    // add_client и локальной копии в deliverItem.
    static constexpr int kMaxClients = 10;
    // Rate-limit входящих кадров (TEXT/BINARY) на сокет: окно 1 с,
    // не больше 50 кадров/с — флуд клиента не должен заливать event-loop.
    // Обработчики httpd — одна задача: слоты без лока; сброс в
    // add_client/remove_client (remove_client из sender-задачи — гонка
    // только на сбросе, приемлема: худшее — окно не сброшено).
    static constexpr uint32_t kMaxInboundFramesPerSec = 50;
    // Глубина очереди отправки (слотов под указатели). Payload копируется
    // в heap: 8 * kMaxWsMessageLen (~8 КБ) = ~64 КБ PSRAM в худшем случае.
    static constexpr size_t kTxQueueDepth = 8;
    // Стек sender-задачи: httpd_ws_send_frame_async -> lwip send, без
    // крупных локальных буферов — 4 КБ с запасом.
    static constexpr uint32_t kSenderTaskStack = 4096;
    // Таймаут ожидания выхода sender-задачи: больше SO_SNDTIMEO httpd (5 с),
    // чтобы не убить задачу внутри send (паттерн DnsServer::stop).
    static constexpr uint32_t kSenderStopTimeoutMs = 6000;

    // Item очереди отправки: СВОЯ копия сообщения — payload события
    // WS_MESSAGE_SEND валиден только внутри колбэка подписки (event-loop
    // освобождает его сразу после возврата из onWsMessageSend).
    struct WsTxItem {
        int     sockfd;   // -1 = broadcast
        size_t  len;
        uint8_t data[];
    };

    AppContext* ctx_;
    std::atomic<httpd_handle_t> server_{nullptr};
    int connected_clients_[kMaxClients];
    int client_count_ = 0;

    // Слот окна rate-limit на сокет: {sockfd, начало окна (мс), кадров в окне}.
    struct RxRateSlot {
        int      sockfd   = -1;
        uint32_t windowMs = 0;
        uint32_t count    = 0;
    };
    RxRateSlot rxRate_[kMaxClients]{};
    // Подписка на WS_MESSAGE_SEND выполняется один раз за время жизни объекта
    // (reg() может вызываться многократно — рестарт httpd в NetworkController).
    bool subs_registered_ = false;

    // Список клиентов под мьютексом (раньше — taskENTER_CRITICAL: критсекция
    // запрещает прерывания на ядре и не сериализует чужие задачи).
    std::mutex clientMux_;

    // Отправка вынесена из event-loop в отдельную задачу: send() с
    // SO_SNDTIMEO=5 с на мёртвом клиенте стопорил бы всю шину событий.
    QueueHandle_t      txQueue_       = nullptr;
    SemaphoreHandle_t  senderDoneSem_ = nullptr;
    TaskHandle_t       senderTask_    = nullptr;
    std::atomic<bool>  senderStop_{false};
    std::atomic<uint32_t> txSent_{0};    // успешно отправленные кадры
    std::atomic<uint32_t> txDrops_{0};   // дропы очереди/копий
    std::atomic<uint32_t> rxDrops_{0};   // дропы rate-limit входящих кадров

    // Подписка на app_event_id_t::WS_MESSAGE_SEND (через EventManager)
    void onWsMessageSend(const ws_message_t* msg);

    // HTTP/WS handler (статический — вызывается httpd-сервером)
    static esp_err_t ws_handler(httpd_req_t* req);

    // WS post-handshake callback (IDF >= 5.5: HTTP GET handler больше не вызывается,
    // поэтому клиент регистрируем здесь, после завершения хэндшейка)
    static esp_err_t onPostHandshake(httpd_req_t* req);

    // Sender-задача: тело цикла + trampoline.
    static void senderTrampoline(void* arg);
    void senderLoop();
    WsTxItem* makeTxItem(const ws_message_t* msg);
    void deliverItem(const WsTxItem* item);
    void sendToSock(httpd_handle_t server, int sockfd,
                    httpd_ws_frame_t* pkt);

    void add_client(int sockfd);
    void remove_client(int sockfd);
    void cleanup_clients();
    // Rate-limit входа: true — кадр принимаем; false — окно переполнено
    // (дроп). Слот заведение лениво при первом кадре сокета.
    bool allowInboundFrame(int sockfd);
    void resetRxRate(int sockfd);
    // Рассылка broadcast-item'а: локальная копия списка сокетов под локом,
    // отправка вне лока (вызывается только из sender-задачи).
    void send_to_all_clients(const char* data, size_t len);
};
