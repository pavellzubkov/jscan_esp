#pragma once
#include "AppContext.hpp"
#include "J1939Proto.hpp"
#include "SnapshotAccumulator.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <array>
#include <atomic>

// Модуль сканера поверх универсального J1939System.
//
// Получает от J1939 готовые сообщения через AppContext::j1939Msg
// (SPSC-очередь, не event loop — горячий поток PGN), накапливает их в
// SnapshotAccumulator (карта (sa,pgn), periodMs, LRU, TTL) и по таймеру
// публикует батч J1939_SNAPSHOT_SEND (как и раньше — безусловно; гейт
// «аудитории нет» в J1939Channel/Comm).
//
// Команды клиентов (SCANNER_REQUEST, редкие) принимает и форвардит в
// J1939System событием J1939_REQUEST — будущая точка расширения политики
// сканирования (rate-limit, периодический опрос) здесь же.
//
// Поля домена SNAPSHOT (snapshotIntervalMs/TtlMs/maxTrackedPgns/activePgns)
// пишет владелец-сканер; UID/домен полей не менялись.
class J1939Scanner {
public:
    explicit J1939Scanner(AppContext* ctx);
    ~J1939Scanner();

    esp_err_t begin();

private:
    AppContext* ctx_;
    SnapshotAccumulator acc_;
    TaskHandle_t task_ = nullptr;

    // Крупные рабочие буферы — члены класса, а не стек задачи (~2.5 КБ:
    // порядок сортировки 128 индексов = 512 Б, батч записей ~2 КБ).
    // Используются только из задачи scanner (она одна) — синхронизация
    // не нужна. См. kTaskStackSize.
    std::array<size_t, SnapshotAccumulator::kMaxRecords> snapOrder_{};
    std::array<J1939Proto::BatchRecord, SnapshotAccumulator::kMaxRecords> snapBatch_{};

    // Graceful shutdown (паттерн J1939System/DnsServer): dtor ставит stop_ ->
    // taskLoop выходит -> give doneSem_ ПЕРЕД vTaskDelete(nullptr); dtor ждёт
    // семафор с таймаутом, fallback — принудительный vTaskDelete.
    std::atomic<bool> stop_{false};
    SemaphoreHandle_t doneSem_ = nullptr;

    // Команда клиента (SCANNER_REQUEST): форвард в J1939System
    // событием J1939_REQUEST (редкое — event loop подходит).
    void onScannerRequest(const j1939_request_t* req);
    // Публикация runtime-поля activePgns (вызывается в момент снапшота).
    void publishActivePgns();
    // Сборка и публикация батча снапшота (перенесено из J1939System).
    void sendSnapshot(uint32_t nowMs);

    static void taskWrapper(void* p);
    void taskLoop();
};