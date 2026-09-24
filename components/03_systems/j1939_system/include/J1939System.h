#pragma once
#include "AppContext.h"
#include "J1939Decoder.h"
#include "J1939TransportProtocol.h"
#include "SnapshotAccumulator.h"
#include "TwaiDriver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

    // Обработчик команды клиента «запросить PGN» (J1939_REQUEST)
    void onJ1939Request(const j1939_request_t* req);

    static void taskWrapper(void* p);
    void taskLoop();

    void processFrame(const TwaiDriver::RxFrame& frame, uint32_t nowMs);
    void processAssembled(const J1939AssembledMsg& msg, uint32_t nowMs);
    void sendSnapshot(uint32_t nowMs);
};