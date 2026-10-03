#include "J1939Scanner.hpp"

#include "LogicUtils.hpp"
#include "SystemTiming.hpp"
#include "esp_log.h"
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {
const char* TAG = "scanner";

// Стек задачи: крупные буферы (порядок/батч снапшота ≈2.5 КБ) — члены
// класса; на стеке только локальные мелочи, фреймы вызовов и логи.
constexpr uint16_t kTaskStackSize = 4096;
constexpr uint8_t  kTaskPriority  = 6;   // ниже j1939 (8), выше event loop (5)
constexpr uint8_t  kTaskCore      = 0;   // ядро 0 безопасно для ESP32 и ESP32-S3
}

J1939Scanner::J1939Scanner(AppContext* ctx)
    : ctx_(ctx)
{
}

J1939Scanner::~J1939Scanner()
{
    // Сначала закрываем канал: продьюсер (J1939) перестаёт класть —
    // иначе drain очереди гонялся бы с новыми сообщениями.
    if (ctx_)
        ctx_->j1939Msg.setConsumer(false);

    // Graceful stop задачи (паттерн J1939System): НЕ vTaskDelete в лоб —
    // стоп-флаг: taskLoop выходит по while (!stop_), даёт doneSem_ ПЕРЕД
    // vTaskDelete(nullptr); dtor ждёт до 2 с, fallback — принудительное
    // удаление.
    if (task_)
    {
        stop_.store(true);
        if (doneSem_ &&
            (xSemaphoreTake(doneSem_, 2000 / portTICK_PERIOD_MS) == pdTRUE ||
             xSemaphoreTake(doneSem_, 100 / portTICK_PERIOD_MS) == pdTRUE))
        {
            task_ = nullptr;   // задача удалила себя сама — хэндл трогать нельзя
        }
        else
        {
            ESP_LOGE(TAG, "scanner task did not stop in time, forcing delete");
            vTaskDelete(task_);
            task_ = nullptr;
        }
    }
    if (ctx_)
        ctx_->events.unsubscribe(this);
    if (doneSem_)
    {
        vSemaphoreDelete(doneSem_);
        doneSem_ = nullptr;
    }
}

esp_err_t J1939Scanner::begin()
{
    // Подписки: отказ (пул EventManager исчерпан) = отказ модуля. makeModule
    // удалит объект при ошибке begin(), dtor снимет подписки и закроет канал.
    if (!subscribeEvent<app_event_id_t::SCANNER_REQUEST>(
            ctx_->events, &J1939Scanner::onScannerRequest, this))
    {
        ESP_LOGE(TAG, "event subscribe failed, abort start");
        return ESP_FAIL;
    }

    // Семафор завершения — до создания задачи (паттерн J1939System).
    stop_.store(false);
    doneSem_ = xSemaphoreCreateBinary();
    if (!doneSem_)
    {
        ESP_LOGE(TAG, "done semaphore create failed");
        return ESP_FAIL;
    }

    // Потребитель канала — до старта задачи, чтобы первый же message
    // из J1939 не дропнулся «consumer not active».
    ctx_->j1939Msg.setConsumer(true);

    if (xTaskCreatePinnedToCore(taskWrapper, "scanner", kTaskStackSize, this,
                                kTaskPriority, &task_, kTaskCore) != pdPASS)
    {
        ESP_LOGE(TAG, "task create failed");
        ctx_->j1939Msg.setConsumer(false);
        vSemaphoreDelete(doneSem_);
        doneSem_ = nullptr;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

void J1939Scanner::onScannerRequest(const j1939_request_t* req)
{
    if (!req)
        return;
    // Редкая команда → event loop (J1939System кладёт её в reqQueue_ и
    // отправляет RQST из своей задачи).
    postEvent<app_event_id_t::J1939_REQUEST>(ctx_->events, *req);
}

void J1939Scanner::taskWrapper(void* p)
{
    static_cast<J1939Scanner*>(p)->taskLoop();
}

void J1939Scanner::taskLoop()
{
    // Все времена в миллисекундах (Timing::nowMs) — тики не используются,
    // чтобы не зависеть от CONFIG_FREERTOS_HZ. Сравнения идут по (int32_t)
    // поверх uint32_t-счётчика мс — переполнение каждые ~49 суток безопасно.
    uint32_t snapshotIntervalMs = Timing::kSnapshotIntervalMs;
    ctx_->fields.getByUid(snapshotIntervalMs_UID, snapshotIntervalMs);

    const uint32_t t0 = Timing::nowMs();
    uint32_t nextSnapshot = t0 + snapshotIntervalMs;

    // Выход по stop_ (dtor): give семафора ДО самоудаления — dtor ждёт take.
    // Ожидание в pop не дольше waitMs (до ближайшего снапшота) — dtor
    // укладывается в таймаут 2000 мс.
    while (!stop_.load())
    {
        // Дренаж: обрабатываем ВСЕ накопившиеся сообщения (timeout 0),
        // иначе при плотной шине глубина очереди (32) исчерпывается быстрее,
        // чем одна итерация цикла успевает взять сообщение.
        j1939_msg_t msg;
        while (ctx_->j1939Msg.pop(&msg, 0))
        {
            acc_.update(msg.sa, msg.pgn, msg.data(), msg.len, msg.tsMs);
            std::free(msg.bigData);   // данные скопированы в аккумулятор
            msg.bigData = nullptr;
        }

        uint32_t now = Timing::nowMs();
        int32_t dSnap = static_cast<int32_t>(nextSnapshot - now);
        const uint32_t waitMs = (dSnap < 1) ? 1 : static_cast<uint32_t>(dSnap);

        // Ждём ближайшее сообщение/таймер; получили сообщение — дренаж всех.
        if (ctx_->j1939Msg.pop(&msg, waitMs))
        {
            do
            {
                acc_.update(msg.sa, msg.pgn, msg.data(), msg.len, msg.tsMs);
                std::free(msg.bigData);
                msg.bigData = nullptr;
            } while (ctx_->j1939Msg.pop(&msg, 0));
        }

        now = Timing::nowMs();
        if ((int32_t)(now - nextSnapshot) >= 0)   // тик-безопасное сравнение
        {
            sendSnapshot(now);
            publishActivePgns();
            ctx_->fields.getByUid(snapshotIntervalMs_UID, snapshotIntervalMs);
            nextSnapshot = now + snapshotIntervalMs;
        }
    }

    // Задача завершается сама: dtor уже ждёт doneSem_ — хэндл трогать нельзя.
    if (doneSem_)
        xSemaphoreGive(doneSem_);
    vTaskDelete(nullptr);
}

void J1939Scanner::publishActivePgns()
{
    // updateField<T>(): diff внутри — PUSH (COMMUNICATION_SEND) уходит только
    // при реальном изменении, писатели AppData не блокируются на отправке.
    updateField<uint16_t>(ctx_, activePgns_UID,
                          static_cast<uint16_t>(acc_.count()));
}

void J1939Scanner::sendSnapshot(uint32_t nowMs)
{
    // Публикация безусловна: гейт «аудитории нет» живёт в J1939Channel
    // (Comm) — здесь только домен: сбор активных записей и батч.
    const SnapshotAccumulator::Record* recs = nullptr;
    uint32_t ttlMs = Timing::kSnapshotTtlMs;
    ctx_->fields.getByUid(snapshotTtlMs_UID, ttlMs);
    const size_t n = acc_.collect(recs, nowMs, ttlMs);
    if (n == 0)
        return;   // нет активных записей — пустой батч не шлём

    // Потолок карты аккумулятора (поле maxTrackedPgns, 16..128): применяется
    // к уже отсортированному порядку, т.е. «свежие первыми» (PROTOCOL §7).
    uint16_t maxTracked = SnapshotAccumulator::kMaxRecords;
    ctx_->fields.getByUid(maxTrackedPgns_UID, maxTracked);
    const size_t emitLimit =
        (n < static_cast<size_t>(maxTracked)) ? n : static_cast<size_t>(maxTracked);

    // Порядок записей в батче — «свежие первыми»: сортируем индексы по
    // lastTsMs (убывание), чтобы при усечении терялись самые старые записи
    // (PROTOCOL §7). n ≤ kMaxRecords, сортировка вставками — без кучи.
    // Массивы — члены snapOrder_/snapBatch_ (~2.5 КБ не помещались на стеке
    // задачи с запасом); используется только из taskLoop (задача одна).
    for (size_t i = 0; i < n; ++i)
        snapOrder_[i] = i;
    for (size_t i = 1; i < n; ++i)
    {
        const size_t key = snapOrder_[i];
        size_t j = i;
        while (j > 0 && recs[snapOrder_[j - 1]].lastTsMs < recs[key].lastTsMs)
        {
            snapOrder_[j] = snapOrder_[j - 1];
            --j;
        }
        snapOrder_[j] = key;
    }

    // Записи батча ссылаются на данные аккумулятора без копирования.
    size_t batchCount = 0;
    size_t payloadLen = 0;
    for (size_t k = 0; k < emitLimit; ++k)
    {
        const SnapshotAccumulator::Record& r = recs[snapOrder_[k]];
        J1939Proto::BatchRecord b;
        b.sa = r.sa;
        b.pgn = r.pgn;
        b.len = r.len;
        b.data = r.data();
        b.periodMs = r.periodMs;

        // Усечение: запись кладём только если батч ещё влезает в лимит payload.
        const size_t add = J1939Proto::batchPayloadSize(&b, 1);
        if (payloadLen + add > J1939Proto::kMaxBatchPayload)
            break;   // остальные (более старые) отбрасываем
        snapBatch_[batchCount++] = b;
        payloadLen += add;
    }
    if (batchCount == 0)
        return;

    const size_t evSize = sizeof(j1939_snapshot_t) + payloadLen;
    // malloc + unique_ptr с deleter free: батч (до ~8 КБ) и так не на стеке,
    // а RAII освобождает буфер на всех путях выхода.
    std::unique_ptr<j1939_snapshot_t, decltype(&std::free)> snap(
        static_cast<j1939_snapshot_t*>(std::malloc(evSize)), &std::free);
    if (!snap)
        return;

    snap->length = payloadLen;
    J1939Proto::serializeBatch(snap->data, payloadLen,
                               snapBatch_.data(), batchCount);

    postSizedEvent<app_event_id_t::J1939_SNAPSHOT_SEND>(
        ctx_->events, snap.get(), evSize);
    // postSizedEvent скопировал данные в очередь event loop'а → буфер здесь
    // больше не нужен; освободит unique_ptr.
}