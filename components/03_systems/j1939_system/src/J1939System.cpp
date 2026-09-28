#include "J1939System.h"

#include "HardwareConfig.h"
#include "LogicUtils.h"
#include "SystemTiming.h"
#include "esp_log.h"
#include <cstdlib>
#include <cstring>

namespace {
const char* TAG = "j1939_sys";

// Стек задачи: в taskLoop на стеке живут J1939AssembledMsg (1785 байт) и
// массив J1939Proto::BatchRecord (до 2 КБ при kMaxRecords=128).
constexpr uint16_t kTaskStackSize = 6144;
constexpr uint8_t  kTaskPriority  = 8;
constexpr uint8_t  kTaskCore      = 0;   // ядро 0 безопасно для ESP32 и ESP32-S3

// Период публикации runtime-полей TWAI (twai*, activePgns).
constexpr uint32_t kTelemetryPeriodMs = 1000;

// Битрейт применяется драйвером TWAI только из этого набора (см. canBitrate).
constexpr uint32_t kAllowedBitrates[] = {125000, 250000, 500000, 1000000};

bool isValidBitrate(uint32_t br)
{
    for (uint32_t b : kAllowedBitrates)
        if (b == br) return true;
    return false;
}
}

J1939System::J1939System(AppContext* ctx)
    : ctx_(ctx)
{
}

J1939System::~J1939System()
{
    if (task_)
    {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    if (ctx_)
        ctx_->events.unsubscribe(this);
    twai_.end();
    if (reqQueue_)
    {
        vQueueDelete(reqQueue_);
        reqQueue_ = nullptr;
    }
}

esp_err_t J1939System::begin()
{
    TwaiDriver::Config cfg;
    cfg.tx = Hw::kCanTxGpio;
    cfg.rx = Hw::kCanRxGpio;
    if (!ctx_->fields.getByName("canBitrate", cfg.bitrate))
        cfg.bitrate = Hw::kCanBitrate;
    if (!isValidBitrate(cfg.bitrate))
    {
        ESP_LOGW(TAG, "canBitrate=%lu invalid (need 125000/250000/500000/1000000), using default 250000",
                 (unsigned long)cfg.bitrate);
        cfg.bitrate = Hw::kCanBitrate;
        // Откатываем поле и уведомляем автосейв, чтобы файл конфига не хранил
        // невалидное значение (в начале работы подписок ещё нет — постим сами).
        ctx_->fields.writeFieldScalar(canBitrate_UID, cfg.bitrate);
        postFieldChanged(ctx_, canBitrate_UID);
        sendField(ctx_, canBitrate_UID);
    }

    esp_err_t err = twai_.begin(cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "TWAI begin failed: %s", esp_err_to_name(err));
        return err;
    }

    ctx_->events.subscribe(APP_EVENTS_BASE, app_event_id_t::J1939_REQUEST,
                           &J1939System::onJ1939Request, this);
    ctx_->events.subscribe(APP_EVENTS_BASE, app_event_id_t::CONFIG_CHANGED,
                           &J1939System::onConfigChanged, this);
    ctx_->events.subscribe(APP_EVENTS_BASE, app_event_id_t::WS_CLIENT_CONNECTED,
                           &J1939System::onWsClientConnected, this);
    ctx_->events.subscribe(APP_EVENTS_BASE, app_event_id_t::WS_CLIENT_DISCONNECTED,
                           &J1939System::onWsClientDisconnected, this);

    reqQueue_ = xQueueCreate(8, sizeof(j1939_request_t));
    if (!reqQueue_)
    {
        ESP_LOGE(TAG, "request queue create failed");
        twai_.end();
        return ESP_FAIL;
    }

    if (xTaskCreatePinnedToCore(taskWrapper, "j1939", kTaskStackSize, this,
                                kTaskPriority, &task_, kTaskCore) != pdPASS)
    {
        ESP_LOGE(TAG, "task create failed");
        twai_.end();
        if (reqQueue_)
        {
            vQueueDelete(reqQueue_);
            reqQueue_ = nullptr;
        }
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

void J1939System::onJ1939Request(const j1939_request_t* req)
{
    if (!req || !reqQueue_)
        return;

    // Event-loop задача не блокируется: только ставим запрос в очередь,
    // отправка (TX до canTxTimeoutMs) выполняется в taskLoop.
    if (xQueueSend(reqQueue_, req, 0) != pdTRUE)
        ESP_LOGW(TAG, "request queue full, dropping RQST pgn=%lu dst=%u",
                 (unsigned long)req->pgn, req->dstAddr);
}

void J1939System::sendRequest(const j1939_request_t& req)
{
    ESP_LOGI(TAG, "RQST pgn=%lu dst=%u", (unsigned long)req.pgn, req.dstAddr);

    // RQST (PGN 59904): priority 6, src — конфигурируемый адрес узла (canNodeAddr).
    // RQST — peer-to-peer: адрес назначения в PS (биты 8..15).
    uint8_t buf[3];
    buf[0] = static_cast<uint8_t>(req.pgn & 0xFF);
    buf[1] = static_cast<uint8_t>((req.pgn >> 8) & 0xFF);
    buf[2] = static_cast<uint8_t>((req.pgn >> 16) & 0xFF);

    uint8_t nodeAddr = 25;   // дефолт canNodeAddr (см. TwaiFields.inc)
    ctx_->fields.getByName("canNodeAddr", nodeAddr);

    uint32_t id = (6u << 26) | (Hw::kPgnRequest << 8) | nodeAddr;
    id = (id & 0xFFFF00FFu) | (static_cast<uint32_t>(req.dstAddr) << 8);

    uint16_t txTimeoutMs = 100;
    ctx_->fields.getByName("canTxTimeoutMs", txTimeoutMs);

    twai_.transmit(id, buf, sizeof(buf), pdMS_TO_TICKS(txTimeoutMs));
}

void J1939System::onWsClientConnected(const ws_message_t* /*msg*/)
{
    ++wsClients_;
}

void J1939System::onWsClientDisconnected(const ws_message_t* /*msg*/)
{
    if (wsClients_.load() > 0)
        --wsClients_;
}

void J1939System::onConfigChanged(const field_change_event_t* evt)
{
    if (!evt)
        return;

    switch (evt->uid)
    {
    case canNodeAddr_UID:
    {
        uint8_t v = 25;
        ctx_->fields.getByName("canNodeAddr", v);
        ESP_LOGI(TAG, "canNodeAddr=%u applied immediately", v);
        break;
    }
    case canTxTimeoutMs_UID:
    {
        uint16_t v = 100;
        ctx_->fields.getByName("canTxTimeoutMs", v);
        ESP_LOGI(TAG, "canTxTimeoutMs=%u applied immediately", v);
        break;
    }
    case canBitrate_UID:
    {
        uint32_t br = 0;
        if (!ctx_->fields.getByName("canBitrate", br))
            break;
        if (!isValidBitrate(br))
        {
            ESP_LOGW(TAG, "canBitrate=%lu invalid (need 125000/250000/500000/1000000), reverting to 250000",
                     (unsigned long)br);
            ctx_->fields.writeFieldScalar(canBitrate_UID, Hw::kCanBitrate);
            postFieldChanged(ctx_, canBitrate_UID);   // автосейв сохранит откат
            sendField(ctx_, canBitrate_UID);          // клиент увидит актуальное значение
            break;
        }
        ESP_LOGI(TAG, "canBitrate=%lu will be applied after reboot",
                 (unsigned long)br);
        break;
    }
    default:
        break;
    }
}

void J1939System::taskWrapper(void* p)
{
    static_cast<J1939System*>(p)->taskLoop();
}

void J1939System::taskLoop()
{
    // Все времена в миллисекундах (Timing::nowMs) — тики не используются,
    // чтобы не зависеть от CONFIG_FREERTOS_HZ. Сравнения идут по (int32_t)
    // поверх uint32_t-счётчика мс — переполнение каждые ~49 суток безопасно.
    uint32_t snapshotIntervalMs = Timing::kSnapshotIntervalMs;
    ctx_->fields.getByName("snapshotIntervalMs", snapshotIntervalMs);

    const uint32_t t0 = Timing::nowMs();
    uint32_t nextSnapshot  = t0 + snapshotIntervalMs;
    uint32_t nextTelemetry = t0 + kTelemetryPeriodMs;

    while (1)
    {
        // RQST от клиентов обрабатываем в своей задаче: TX может блокировать
        // до canTxTimeoutMs — нельзя делать это в event-loop задаче.
        j1939_request_t req;
        while (xQueueReceive(reqQueue_, &req, 0) == pdTRUE)
            sendRequest(req);

        uint32_t now = Timing::nowMs();
        uint32_t waitMs = Timing::computeWaitMs(nextSnapshot, nextTelemetry,
                                                now);

        TwaiDriver::RxFrame* frame = nullptr;
        if (xQueueReceive(twai_.rxReadyQueue(), &frame,
                          pdMS_TO_TICKS(waitMs)) == pdTRUE)
        {
            processFrame(*frame, Timing::nowMs());
            xQueueSend(twai_.rxFreeQueue(), &frame, 0);   // вернуть слот
        }

        now = Timing::nowMs();
        if ((int32_t)(now - nextSnapshot) >= 0)   // тик-безопасное сравнение
        {
            sendSnapshot(now);
            ctx_->fields.getByName("snapshotIntervalMs", snapshotIntervalMs);
            nextSnapshot = now + snapshotIntervalMs;
        }
        if ((int32_t)(now - nextTelemetry) >= 0)
        {
            updateTwaiStatus();
            nextTelemetry = now + kTelemetryPeriodMs;
        }
    }
}

void J1939System::updateTwaiStatus()
{
    AppDataLock dataLock(ctx_);   // атомарная публикация блока runtime-полей

    TwaiDriver::Status st;
    if (!twai_.getStatus(st))
    {
        // Драйвер не создан/остановлен — публикуем STOPPED.
        ctx_->fields.writeFieldScalar(twaiState_UID, static_cast<uint8_t>(0));
        ctx_->fields.writeFieldScalar(twaiStarted_UID, false);
        sendField(ctx_, twaiState_UID);
        sendField(ctx_, twaiStarted_UID);
        return;
    }

    ctx_->fields.writeFieldScalar(twaiStarted_UID, true);
    ctx_->fields.writeFieldScalar(twaiTxErr_UID, static_cast<uint8_t>(st.txErr));
    ctx_->fields.writeFieldScalar(twaiRxErr_UID, static_cast<uint8_t>(st.rxErr));

    if (st.state == TWAI_ERROR_BUS_OFF)
    {
        ctx_->fields.writeFieldScalar(twaiState_UID, static_cast<uint8_t>(2));  // BUS_OFF

        bool canAutoRecover = false;
        ctx_->fields.getByName("canAutoRecover", canAutoRecover);
        if (canAutoRecover)
        {
            ESP_LOGW(TAG, "BUS_OFF detected, recovering");
            twai_.recover();
            ++twaiRecoverCount_;
            ctx_->fields.writeFieldScalar(twaiRecoverCount_UID, twaiRecoverCount_);
            // Следующий опрос покажет реальное состояние после recover.
            ctx_->fields.writeFieldScalar(twaiState_UID, static_cast<uint8_t>(3));  // RECOVERING
        }
    }
    else
    {
        // ACTIVE/WARNING/PASSIVE — узел в сети (RUNNING).
        ctx_->fields.writeFieldScalar(twaiState_UID, static_cast<uint8_t>(1));
    }

    sendField(ctx_, twaiState_UID);
    sendField(ctx_, twaiTxErr_UID);
    sendField(ctx_, twaiRxErr_UID);
    sendField(ctx_, twaiStarted_UID);
    sendField(ctx_, twaiRecoverCount_UID);

    ctx_->fields.writeFieldScalar(activePgns_UID,
                                  static_cast<uint16_t>(acc_.count()));
    sendField(ctx_, activePgns_UID);
}

void J1939System::processFrame(const TwaiDriver::RxFrame& frame, uint32_t nowMs)
{
    const J1939PgnMsg m = J1939Decoder::decode(frame);

    if (m.pgn == Hw::kPgnTpCm)
    {
        tp_.onTpCm(m);
    }
    else if (m.pgn == Hw::kPgnTpDt)
    {
        J1939AssembledMsg out = {};
        if (tp_.onTpDt(m, out))
            processAssembled(out, nowMs);
    }
    else
    {
        acc_.update(m.sa, m.pgn, m.data, m.dlc, nowMs);
    }
}

void J1939System::processAssembled(const J1939AssembledMsg& msg, uint32_t nowMs)
{
    acc_.update(msg.sa, msg.pgn, msg.data, msg.len, nowMs);
}

void J1939System::sendSnapshot(uint32_t nowMs)
{
    if (wsClients_.load() == 0)
        return;   // нет WS-клиентов — батч не строим и не аллоцируем

    const SnapshotAccumulator::Record* recs = nullptr;
    uint32_t ttlMs = Timing::kSnapshotTtlMs;
    ctx_->fields.getByName("snapshotTtlMs", ttlMs);
    const size_t n = acc_.collect(recs, nowMs, ttlMs);
    if (n == 0)
        return;   // нет активных записей — пустой батч не шлём

    // Порядок записей в батче — «свежие первыми»: сортируем индексы по
    // lastTsMs (убывание), чтобы при усечении терялись самые старые записи
    // (PROTOCOL §7). n ≤ kMaxRecords, сортировка вставками — без кучи.
    size_t order[SnapshotAccumulator::kMaxRecords];
    for (size_t i = 0; i < n; ++i)
        order[i] = i;
    for (size_t i = 1; i < n; ++i)
    {
        const size_t key = order[i];
        size_t j = i;
        while (j > 0 && recs[order[j - 1]].lastTsMs < recs[key].lastTsMs)
        {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = key;
    }

    // Записи батча ссылаются на данные аккумулятора без копирования.
    J1939Proto::BatchRecord batch[SnapshotAccumulator::kMaxRecords];
    size_t batchCount = 0;
    size_t payloadLen = 0;
    for (size_t k = 0; k < n; ++k)
    {
        const SnapshotAccumulator::Record& r = recs[order[k]];
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
        batch[batchCount++] = b;
        payloadLen += add;
    }
    if (batchCount == 0)
        return;

    const size_t evSize = sizeof(j1939_snapshot_t) + payloadLen;
    j1939_snapshot_t* snap =
        static_cast<j1939_snapshot_t*>(malloc(evSize));
    if (!snap)
        return;

    snap->length = payloadLen;
    J1939Proto::serializeBatch(snap->data, payloadLen, batch, batchCount);

    ctx_->events.postSized(APP_EVENTS_BASE,
                           app_event_id_t::J1939_SNAPSHOT_SEND,
                           snap, evSize);
    free(snap);
}