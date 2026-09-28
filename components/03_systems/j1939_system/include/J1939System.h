#pragma once
#include "AppContext.h"
#include "J1939Decoder.h"
#include "J1939TransportProtocol.h"
#include "SnapshotAccumulator.h"
#include "TwaiDriver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <atomic>

// Координатор J1939: владеет драйвером TWAI, декодером, TP и аккумулятором.
// Одна задача: приём кадров + публикация снапшотов по таймеру.
class J1939System {
public:
    explicit J1939System(AppContext* ctx);
    ~J1939System();

    esp_err_t begin();

private:
    AppContext* ctx_;
    TwaiDriver twai_;
    J1939TransportProtocol tp_;
    SnapshotAccumulator acc_;
    TaskHandle_t task_ = nullptr;

    // Очередь RQST от клиентов (J1939_REQUEST): событие (event-loop) только
    // ставит запрос, TX с блокировкой до canTxTimeoutMs выполняется в taskLoop.
    QueueHandle_t reqQueue_ = nullptr;

    uint32_t twaiRecoverCount_ = 0;   // число авто-recover после BUS_OFF

    // Число подключённых WS-клиентов: при 0 не строим и не шлём батч.
    std::atomic<int> wsClients_{0};

    // Обработчик команды клиента «запросить PGN» (J1939_REQUEST): только
    // кладёт запрос в reqQueue_ (не блокирует event loop).
    void onJ1939Request(const j1939_request_t* req);
    // Фактическая отправка RQST (вызывается из taskLoop).
    void sendRequest(const j1939_request_t& req);
    // Счётчик WS-клиентов (подписка на WS_CLIENT_CONNECTED/DISCONNECTED).
    void onWsClientConnected(const ws_message_t* msg);
    void onWsClientDisconnected(const ws_message_t* msg);
    // Применение TWAI-конфига по CONFIG_CHANGED (canNodeAddr/canTxTimeoutMs сразу,
    // canBitrate — после перезагрузки, с валидацией набора {125/250/500/1000} кбит/с).
    void onConfigChanged(const field_change_event_t* evt);
    // Публикация runtime-полей twai* и activePgns (~1 раз в секунду).
    void updateTwaiStatus();

    static void taskWrapper(void* p);
    void taskLoop();

    void processFrame(const TwaiDriver::RxFrame& frame, uint32_t nowMs);
    void processAssembled(const J1939AssembledMsg& msg, uint32_t nowMs);
    void sendSnapshot(uint32_t nowMs);
};