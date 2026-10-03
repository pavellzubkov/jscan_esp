#include "J1939System.h"

#include "HardwareConfig.h"
#include "LogicUtils.h"
#include "SystemTiming.h"
#include "esp_log.h"
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {
const char* TAG = "j1939_sys";

// Стек задачи: крупные буферы (порядок/батч снапшота ≈2.5 КБ, буфер сборки
// TP 1785 байт) вынесены в члены класса — на стеке только локальные мелочи,
// фреймы вызовов и логи. 8192 — с запасом на фреймы/FreeRTOS/вложенность.
constexpr uint16_t kTaskStackSize = 8192;
constexpr uint8_t  kTaskPriority  = 8;
constexpr uint8_t  kTaskCore      = 0;   // ядро 0 безопасно для ESP32 и ESP32-S3

// Период публикации runtime-полей TWAI (twai*, activePgns).
constexpr uint32_t kTelemetryPeriodMs = 1000;

// Битрейт применяется драйвером TWAI только из этого набора (см. canBitrate).
constexpr uint32_t kAllowedBitrates[] = {125000, 250000, 500000, 1000000};

// Значения runtime-поля twaiState (TwaiFields.inc: «0=STOPPED 1=RUNNING
// 2=BUS_OFF 3=RECOVERING») — вместо магических чисел в теле.
enum TwaiStatePub : uint8_t {
    kStateStopped   = 0,
    kStateRunning   = 1,
    kStateBusOff    = 2,
    kStateRecovering = 3,
};

bool isValidBitrate(uint32_t br)
{
    for (uint32_t b : kAllowedBitrates)
        if (b == br) return true;
    return false;
}

// Ошибки TWAI драйвер считает в uint32_t, а поля twaiTxErr/twaiRxErr
// описаны диапазоном 0..255 — усекаем, чтобы запись не ушла OUT_OF_RANGE.
uint8_t clampErrCnt(uint32_t v)
{
    return v > 255 ? 255 : static_cast<uint8_t>(v);
}

// Дефолты TwaiDriver::Config (слой 02 намеренно не знает HardwareConfig)
// обязаны совпадать с константами Hw — сверяем на этапе компиляции, чтобы
// дефолт драйвера и конфиг ниже не разъезжались.
constexpr TwaiDriver::Config kTwaiDefaults{};
static_assert(kTwaiDefaults.tx == Hw::kCanTxGpio,
              "TwaiDriver::Config::tx default != Hw::kCanTxGpio");
static_assert(kTwaiDefaults.rx == Hw::kCanRxGpio,
              "TwaiDriver::Config::rx default != Hw::kCanRxGpio");
static_assert(kTwaiDefaults.bitrate == Hw::kCanBitrate,
              "TwaiDriver::Config::bitrate default != Hw::kCanBitrate");
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
    if (!ctx_->fields.getByUid(canBitrate_UID, cfg.bitrate))
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

    // Наш адрес — распознавание RTS «на нас» (TP.CM) в задаче приёма.
    uint8_t nodeAddr = Hw::kDefaultNodeAddr;
    ctx_->fields.getByUid(canNodeAddr_UID, nodeAddr);
    tp_.setLocalAddr(nodeAddr);

    esp_err_t err = twai_.begin(cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "TWAI begin failed: %s", esp_err_to_name(err));
        return err;
    }

    // Подписки: отказ (пул EventManager исчерпан) = отказ модуля. makeModule
    // удалит объект при ошибке begin(), dtor снимет уже зарегистрированные
    // подписки и остановит TWAI — откат делать отдельно не нужно.
    const bool subsOk =
        subscribeEvent<app_event_id_t::J1939_REQUEST>(
            ctx_->events, &J1939System::onJ1939Request, this) &&
        subscribeEvent<app_event_id_t::CONFIG_CHANGED>(
            ctx_->events, &J1939System::onConfigChanged, this);
    if (!subsOk)
    {
        ESP_LOGE(TAG, "event subscribe failed, abort start");
        return ESP_FAIL;
    }

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

    uint8_t nodeAddr = Hw::kDefaultNodeAddr;   // дефолт canNodeAddr (TwaiFields.inc)
    ctx_->fields.getByUid(canNodeAddr_UID, nodeAddr);

    uint32_t id = (6u << 26) | (Hw::kPgnRequest << 8) | nodeAddr;
    id = (id & 0xFFFF00FFu) | (static_cast<uint32_t>(req.dstAddr) << 8);

    uint16_t txTimeoutMs = Hw::kDefaultTxTimeoutMs;
    ctx_->fields.getByUid(canTxTimeoutMs_UID, txTimeoutMs);

    // canTxTimeoutMs — ограничение драйвера (поле в конфиге/протоколе),
    // но TX RQST больше НЕ ждёт окончания передачи: timeout=0 = «кадр
    // поставлен в очередь драйвера» (ESP_OK). Блокировка задачи на
    // canTxTimeoutMs открывала окно для дропов RX, пока ISR сыпал кадры.
    // fail_retry_cnt=-1 — драйвер ретранслирует сам.
    esp_err_t txErr = twai_.transmit(id, buf, sizeof(buf), 0);
    if (txErr == ESP_ERR_TIMEOUT)
        ESP_LOGW(TAG, "RQST pgn=%lu tx queue full (canTxTimeoutMs=%u ignored for wait)",
                 (unsigned long)req.pgn, txTimeoutMs);
    else if (txErr != ESP_OK)
        ESP_LOGW(TAG, "RQST pgn=%lu tx failed: %s", (unsigned long)req.pgn,
                 esp_err_to_name(txErr));
}

void J1939System::onConfigChanged(const field_change_event_t* evt)
{
    if (!evt)
        return;

    switch (evt->uid)
    {
    case canNodeAddr_UID:
    {
        uint8_t v = Hw::kDefaultNodeAddr;
        if (!ctx_->fields.getByUid(canNodeAddr_UID, v))
            ESP_LOGW(TAG, "read canNodeAddr failed, applying default");
        ESP_LOGI(TAG, "canNodeAddr=%u applied immediately", v);
        tp_.setLocalAddr(v);   // TP: RTS «на нас» — по новому адресу
        break;
    }
    case canTxTimeoutMs_UID:
    {
        uint16_t v = Hw::kDefaultTxTimeoutMs;
        if (!ctx_->fields.getByUid(canTxTimeoutMs_UID, v))
            ESP_LOGW(TAG, "read canTxTimeoutMs failed, keeping default");
        ESP_LOGI(TAG, "canTxTimeoutMs=%u applied immediately", v);
        break;
    }
    case canBitrate_UID:
    {
        uint32_t br = 0;
        if (!ctx_->fields.getByUid(canBitrate_UID, br))
        {
            ESP_LOGW(TAG, "read canBitrate failed, skipping apply");
            break;
        }
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
    ctx_->fields.getByUid(snapshotIntervalMs_UID, snapshotIntervalMs);

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

        // Дренаж: обрабатываем ВСЕ накопившиеся кадры (timeout 0), иначе
        // при полной шине пул слотов (16) исчерпывается быстрее, чем одна
        // итерация цикла успевает взять кадр — дропы росли бы зря.
        TwaiDriver::RxFrame* frame = nullptr;
        while (xQueueReceive(twai_.rxReadyQueue(), &frame, 0) == pdTRUE)
        {
            processFrame(*frame, Timing::nowMs());
            xQueueSend(twai_.rxFreeQueue(), &frame, 0);   // вернуть слот
        }
        // Кадров нет — ждём ближайшего события/таймера (не дольше waitMs).
        if (xQueueReceive(twai_.rxReadyQueue(), &frame,
                          pdMS_TO_TICKS(waitMs)) == pdTRUE)
        {
            processFrame(*frame, Timing::nowMs());
            xQueueSend(twai_.rxFreeQueue(), &frame, 0);   // вернуть слот
        }

        now = Timing::nowMs();
        tp_.tick(now);   // sweep таймаутов TP-сессий (DT могли прекратиться)
        if ((int32_t)(now - nextSnapshot) >= 0)   // тик-безопасное сравнение
        {
            sendSnapshot(now);
            ctx_->fields.getByUid(snapshotIntervalMs_UID, snapshotIntervalMs);
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
    // Страховочная сетка по стеку: uxTaskGetStackHighWaterMark — минимально
    // свободный стек с момента старта. Вызывается раз в секунду (период
    // телеметрии); < 1 КБ свободного — предупреждение (фреймы/логи могут не
    // влезть), каждый раз в ESP_LOGD — для профилирования.
    if (task_)
    {
        const UBaseType_t hwm = uxTaskGetStackHighWaterMark(task_);
        if (hwm < 1024)
            ESP_LOGW(TAG, "j1939 stack HWM: %u bytes free (task size %u)",
                     unsigned(hwm), unsigned(kTaskStackSize));
        else
            ESP_LOGD(TAG, "j1939 stack HWM: %u bytes free", unsigned(hwm));
    }

    // Публикация через updateField<T>(): AppDataLock удерживается только на
    // чтении+записи, PUSH (COMMUNICATION_SEND → post, до 50 мс) уходит ПОСЛЕ
    // снятия лока — писатели AppData не блокируются на отправке. Diff внутри
    // updateField избавляет от ежесекундного PUSH-спама неизменённых полей.
    TwaiDriver::Status st;
    if (!twai_.getStatus(st))
    {
        // Драйвер не создан/остановлен — публикуем STOPPED.
        updateField<uint8_t>(ctx_, twaiState_UID,
                             static_cast<uint8_t>(kStateStopped));
        updateField<bool>(ctx_, twaiStarted_UID, false);
        updateField<uint32_t>(ctx_, twaiRxDrops_UID, twai_.rxDrops());
        updateField<uint32_t>(ctx_, twaiTxDrops_UID, twai_.txDrops());
        return;
    }

    uint8_t state = static_cast<uint8_t>(kStateRunning);
    if (st.state == TWAI_ERROR_BUS_OFF)
    {
        bool canAutoRecover = false;
        {
            AppDataLock lock(ctx_);   // чтение config-поля — под локом adata
            ctx_->fields.getByUid(canAutoRecover_UID, canAutoRecover);
        }
        if (canAutoRecover)
        {
            ESP_LOGW(TAG, "BUS_OFF detected, recovering");
            twai_.recover();
            ++twaiRecoverCount_;
            // Следующий опрос покажет реальное состояние после recover.
            state = static_cast<uint8_t>(kStateRecovering);
        }
        else
        {
            state = static_cast<uint8_t>(kStateBusOff);
        }
    }
    // ACTIVE/WARNING/PASSIVE — узел в сети (RUNNING).

    updateField<bool>(ctx_, twaiStarted_UID, true);
    updateField<uint8_t>(ctx_, twaiState_UID, state);
    updateField<uint8_t>(ctx_, twaiTxErr_UID, clampErrCnt(st.txErr));
    updateField<uint8_t>(ctx_, twaiRxErr_UID, clampErrCnt(st.rxErr));
    updateField<uint32_t>(ctx_, twaiRecoverCount_UID, twaiRecoverCount_);
    updateField<uint32_t>(ctx_, twaiRxDrops_UID, twai_.rxDrops());
    updateField<uint32_t>(ctx_, twaiTxDrops_UID, twai_.txDrops());
    updateField<uint16_t>(ctx_, activePgns_UID,
                          static_cast<uint16_t>(acc_.count()));
}

void J1939System::processFrame(const TwaiDriver::RxFrame& frame, uint32_t nowMs)
{
    const J1939PgnMsg m = J1939Decoder::decode(frame);

    if (m.pgn == Hw::kPgnTpCm)
    {
        sendTpAction(tp_.onTpCm(m, nowMs));
    }
    else if (m.pgn == Hw::kPgnTpDt)
    {
        // Буфер сборки — член assembled_ (1785+ байт не держим на стеке):
        // onTpDt заполняет его целиком перед возвратом true, переиспользование
        // безопасно (задача одна). action — EOM только при завершении активного
        // приёма; для BAM/пассивного сниффинга остаётся None.
        TpAction act;
        if (tp_.onTpDt(m, nowMs, assembled_, &act))
            processAssembled(assembled_, nowMs);
        sendTpAction(act);
    }
    else
    {
        acc_.update(m.sa, m.pgn, m.data, m.dlc, nowMs);
    }
}

void J1939System::sendTpAction(const TpAction& act)
{
    if (act.kind == TpAction::Kind::None)
        return;

    // TP.CM (PDU1): ID = prio<<26 | PF<<8 | PS(=dst) | SA. По SAE приоритет
    // TP.CM — 7. Буфер: reserved-байты 0xFF по спецификации.
    uint8_t buf[8];
    std::memset(buf, 0xFF, sizeof(buf));
    switch (act.kind)
    {
    case TpAction::Kind::SendCts:
        // CTS: [17, packets-we-can-receive, 0xFF(max), 0xFF, 0xFF, PGN LE]
        buf[0] = 17;
        buf[1] = act.packets;
        break;
    case TpAction::Kind::SendEom:
        // EOM: [19, size LE, total packets, 0xFF, PGN LE]
        buf[0] = 19;
        buf[1] = static_cast<uint8_t>(act.totalLen & 0xFF);
        buf[2] = static_cast<uint8_t>((act.totalLen >> 8) & 0xFF);
        buf[3] = act.totalPackets;
        break;
    default:
        return;
    }
    buf[5] = static_cast<uint8_t>(act.pgn & 0xFF);
    buf[6] = static_cast<uint8_t>((act.pgn >> 8) & 0xFF);
    buf[7] = static_cast<uint8_t>((act.pgn >> 16) & 0xFF);

    uint8_t nodeAddr = Hw::kDefaultNodeAddr;
    ctx_->fields.getByUid(canNodeAddr_UID, nodeAddr);
    uint32_t id = (7u << 26) | (Hw::kPgnTpCm << 8) | nodeAddr;
    id = (id & 0xFFFF00FFu) | (static_cast<uint32_t>(act.dst) << 8);

    // timeout=0: «кадр поставлен в очередь драйвера» = успех — задача приёма
    // не ждёт завершения TX (как RQST, см. sendRequest).
    const char* what = (act.kind == TpAction::Kind::SendCts) ? "CTS" : "EOM";
    esp_err_t txErr = twai_.transmit(id, buf, sizeof(buf), 0);
    if (txErr == ESP_ERR_TIMEOUT)
        ESP_LOGW(TAG, "%s tp pgn=%lu dst=%u tx queue full",
                 what, (unsigned long)act.pgn, act.dst);
    else if (txErr != ESP_OK)
        ESP_LOGW(TAG, "%s tp pgn=%lu dst=%u tx failed: %s",
                 what, (unsigned long)act.pgn, act.dst, esp_err_to_name(txErr));
}

void J1939System::processAssembled(const J1939AssembledMsg& msg, uint32_t nowMs)
{
    acc_.update(msg.sa, msg.pgn, msg.data, msg.len, nowMs);
}

void J1939System::sendSnapshot(uint32_t nowMs)
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
    // а RAII освобождает буфер на всех путях выхода
    // (раньше — ручной free только на успешном пути; ранний return ниже
    // был бы утечкой). Выравнивание max_align_t — под flexible array.
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