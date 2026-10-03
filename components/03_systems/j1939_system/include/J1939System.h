#pragma once
#include "AppContext.h"
#include "J1939Decoder.h"
#include "J1939Proto.h"
#include "J1939TransportProtocol.h"
#include "SnapshotAccumulator.h"
#include "TwaiDriver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <array>

// Координатор J1939: владеет драйвером TWAI, декодером, TP и аккумулятором.
// Одна задача: приём кадров + публикация снапшотов по таймеру.
// О WS-клиентах и доставке не знает: снапшот публикуется безусловно
// (J1939_SNAPSHOT_SEND), решение «слать/не слать» — в J1939Channel (Comm).
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

    // Крупные рабочие буферы — члены класса, а не стек taskLoop (суммарно
    // ~4.3 КБ: порядок сортировки 128 индексов = 512 Б, батч записей ~2 КБ,
    // буфер сборки TP = 1785+ байт). Используются только из задачи J1939
    // (она одна) — синхронизация не нужна. См. kTaskStackSize.
    std::array<size_t, SnapshotAccumulator::kMaxRecords> snapOrder_{};
    std::array<J1939Proto::BatchRecord, SnapshotAccumulator::kMaxRecords> snapBatch_{};
    J1939AssembledMsg assembled_{};

    // Очередь RQST от клиентов (J1939_REQUEST): событие (event-loop) только
    // ставит запрос, TX с блокировкой до canTxTimeoutMs выполняется в taskLoop.
    QueueHandle_t reqQueue_ = nullptr;

    uint32_t twaiRecoverCount_ = 0;   // число авто-recover после BUS_OFF

    // Обработчик команды клиента «запросить PGN» (J1939_REQUEST): только
    // кладёт запрос в reqQueue_ (не блокирует event loop).
    void onJ1939Request(const j1939_request_t* req);
    // Фактическая отправка RQST (вызывается из taskLoop).
    void sendRequest(const j1939_request_t& req);
    // Трансляция TP-действия (CTS/EOM) в кадр TWAI (вызывается из taskLoop).
    void sendTpAction(const TpAction& act);
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