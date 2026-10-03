#pragma once
#include "AppContext.hpp"
#include "J1939Decoder.hpp"
#include "J1939Proto.hpp"
#include "J1939TransportProtocol.hpp"
#include "TwaiDriver.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <array>
#include <atomic>

// Универсальный J1939: владеет драйвером TWAI, декодером и TP-сессиями
// (BAM/RTS/CTS/EOM), шлёт RQST по команде. Одна задача: приём кадров +
// телеметрия TWAI. Готовые сообщения (одиночные кадры и собранные TP)
// публикует в AppContext::j1939Msg (SPSC-очередь) — потребитель и вся
// политика (аккумулятор, TTL, батч, снапшоты) — в J1939Scanner.
// О WS-клиентах и доставке не знает: снапшот публикуется безусловно
// (J1939_SNAPSHOT_SEND, уже сканером), решение «слать/не слать» — в
// J1939Channel (Comm).
class J1939System {
public:
    explicit J1939System(AppContext* ctx);
    ~J1939System();

    esp_err_t begin();

private:
    AppContext* ctx_;
    TwaiDriver twai_;
    J1939TransportProtocol tp_;
    TaskHandle_t task_ = nullptr;

    // Буфер сборки TP — член класса (1785+ байт не помещались бы на стеке
    // taskLoop с запасом); используется только из задачи J1939 (она одна).
    J1939AssembledMsg assembled_{};

    // Очередь RQST от клиентов (J1939_REQUEST): событие (event-loop) только
    // ставит запрос, TX с блокировкой до canTxTimeoutMs выполняется в taskLoop.
    QueueHandle_t reqQueue_ = nullptr;

    // Graceful shutdown (паттерн DnsServer::stop): dtor ставит stop_ ->
    // taskLoop выходит из цикла -> give doneSem_ ПЕРЕД vTaskDelete(nullptr);
    // dtor ждёт семафор с таймаутом, fallback — принудительный vTaskDelete.
    // Задачу нельзя убивать под twai_.transmit (мьютекс навсегда занят) или
    // посреди processFrame — иначе twai_.end() в dtor виснет.
    std::atomic<bool> stop_{false};
    SemaphoreHandle_t doneSem_ = nullptr;

    uint32_t twaiRecoverCount_ = 0;   // число авто-recover после BUS_OFF

    // Bus-off recovery с экспоненциальным backoff (ARCH_FIX4, шаг 2).
    // Вызывается из taskLoop по событию takeBusOffEvent() и как fallback
    // в тик телеметрии; уважает canAutoRecover. Только задача J1939 — без локов.
    static constexpr uint32_t kRecoverBackoffBaseMs = 100;    // первый интервал
    static constexpr uint32_t kRecoverBackoffCapMs  = 30000;  // потолок (×2 до cap)
    static constexpr uint32_t kRecoverStableResetMs = 60000;  // без bus-off → сброс backoff
    uint32_t recoverBackoffMs_   = kRecoverBackoffBaseMs;
    uint32_t nextRecoverAllowedMs_ = 0;   // до этого момента recover запрещён
    uint32_t lastBusOffMs_ = 0;           // момент последнего входа в bus-off
    void tryRecoverBusOff(uint32_t nowMs);

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
    // Публикация runtime-полей twai* (~1 раз в секунду).
    void updateTwaiStatus();

    static void taskWrapper(void* p);
    void taskLoop();

    void processFrame(const TwaiDriver::RxFrame& frame, uint32_t nowMs);
    // Готовое сообщение (кадр/сборка TP) → очередь сканера (j1939Msg).
    void publishMsg(uint32_t sa, uint32_t pgn, const uint8_t* data,
                    size_t len, uint32_t nowMs);
};