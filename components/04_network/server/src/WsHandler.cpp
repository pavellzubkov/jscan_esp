#include "WsHandler.hpp"
#include "SystemTiming.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <cstring>
#include <cstdlib>
#include <memory>

static const char *TAG = "WsHandler";

WsHandler::WsHandler(AppContext* ctx)
    : ctx_(ctx), client_count_(0)
{
    memset(connected_clients_, 0, sizeof(connected_clients_));
}

WsHandler::~WsHandler() {
    // Сначала гасим sender-задачу (она могла бы работать с умирающим
    // объектом: remove_client -> пост события).
    stopTx();
    // Подписка делается в reg(); delete без снятия (stop() при ошибке
    // ServerModule::begin) оставил бы обработчик на удалённый объект → UAF.
    ctx_->events.unsubscribe(this);
    // Дренаж не отправленных items + удаление ресурсов очереди.
    if (txQueue_) {
        WsTxItem* item = nullptr;
        while (xQueueReceive(txQueue_, &item, 0) == pdTRUE) {
            std::free(item);
        }
        vQueueDelete(txQueue_);
        txQueue_ = nullptr;
    }
    if (senderDoneSem_) {
        vSemaphoreDelete(senderDoneSem_);
        senderDoneSem_ = nullptr;
    }
}

void WsHandler::add_client(int sockfd) {
  bool already_connected = false;
  bool rejected_full = false;
  int new_count = 0;
  resetRxRate(sockfd);   // новое соединение — свежее окно rate-limit

  {
    std::lock_guard<std::mutex> lock(clientMux_);

    // Проверяем, есть ли уже такой клиент; при повторе — удаляем старую
    // запись, чтобы добавить актуальную (замена соединения).
    for (int i = 0; i < client_count_; i++) {
      if (connected_clients_[i] == sockfd) {
        already_connected = true;
        for (int j = i; j < client_count_ - 1; j++) {
          connected_clients_[j] = connected_clients_[j + 1];
        }
        client_count_--;
        break;
      }
    }

    if (client_count_ >= kMaxClients) {
      rejected_full = true;
    } else {
      connected_clients_[client_count_++] = sockfd;
      new_count = client_count_;
    }
  }

  // Логирование и посты — ВНЕ мьютекса clientMux_.
  if (rejected_full) {
    ESP_LOGW(TAG, "Max clients reached, rejecting sockfd: %d", sockfd);
    // Запись этого sockfd уже была удалена выше — без DISCONNECTED
    // счётчики аудитории (J1939Channel) уехали бы вверх.
    if (already_connected) {
      ws_message_t dmsg = {};
      dmsg.sockfd = sockfd;
      postEvent<app_event_id_t::WS_CLIENT_DISCONNECTED>(ctx_->events, dmsg);
    }
    // Закрываем сокет, чтобы клиент не висел подключённым без доставки данных.
    if (server_) {
      httpd_sess_trigger_close(server_, sockfd);
    }
    return;
  }

  if (already_connected) {
    ESP_LOGW(TAG, "Client %d already connected — replaced", sockfd);
  }
  ESP_LOGI(TAG, "Client %d added, total: %d (free heap: %u B)", sockfd,
           new_count, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));

  ws_message_t msg = {};
  msg.sockfd = sockfd;
  if (already_connected) {
    // Повторная регистрация того же sockfd: старое соединение закрылось
    // без remove_client — балансируем парой DISCONNECTED/CONNECTED,
    // иначе счётчики аудитории уедут вверх.
    postEvent<app_event_id_t::WS_CLIENT_DISCONNECTED>(ctx_->events, msg);
  }
  postEvent<app_event_id_t::WS_CLIENT_CONNECTED>(ctx_->events, msg);
}

void WsHandler::remove_client(int sockfd) {
  bool removed = false;
  int count_for_log = 0;
  resetRxRate(sockfd);   // сокет уходит — освобождаем слот окна

  {
    std::lock_guard<std::mutex> lock(clientMux_);
    for (int i = 0; i < client_count_; i++) {
      if (connected_clients_[i] == sockfd) {
        for (int j = i; j < client_count_ - 1; j++) {
          connected_clients_[j] = connected_clients_[j + 1];
        }
        client_count_--;
        connected_clients_[client_count_] = 0;
        removed = true;
        break;
      }
    }
    count_for_log = client_count_;
  }

  if (removed) {
    // Лог/пост — вне мьютекса.
    ESP_LOGI(TAG, "Client %d removed, total: %d (free heap: %u B)", sockfd,
             count_for_log, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    ws_message_t msg = {};
    msg.sockfd = sockfd;
    postEvent<app_event_id_t::WS_CLIENT_DISCONNECTED>(ctx_->events, msg);
  }
}

// Rate-limit входящих TEXT/BINARY-кадров: окно 1 с (Timing::nowMs, монотонно,
// переполнение uint32 через ~49 суток корректно через беззнаковый вычет).
// Лок не нужен: обработчики httpd — одна задача (слот только читает
// add/remove, сбрасывает resetRxRate). Слот лениво заводится при первом кадре.
bool WsHandler::allowInboundFrame(int sockfd) {
    const uint32_t now = Timing::nowMs();
    RxRateSlot* freeSlot = nullptr;
    for (auto& s : rxRate_) {
        if (s.sockfd == sockfd) {
            if (static_cast<uint32_t>(now - s.windowMs) >= 1000) {
                s.windowMs = now;
                s.count = 0;
            }
            if (++s.count > kMaxInboundFramesPerSec) {
                return false;
            }
            return true;
        }
        if (!freeSlot && s.sockfd == -1) {
            freeSlot = &s;
        }
    }
    if (!freeSlot) {
        return true;   // слоты кончились — без дедупа лучше, чем глушить кадры
    }
    freeSlot->sockfd = sockfd;
    freeSlot->windowMs = now;
    freeSlot->count = 1;
    return true;
}

void WsHandler::resetRxRate(int sockfd) {
    for (auto& s : rxRate_) {
        if (s.sockfd == sockfd) {
            s = RxRateSlot{};
            return;
        }
    }
}

void WsHandler::cleanup_clients() {
  int removed[kMaxClients];
  int old_count = 0;

  {
    std::lock_guard<std::mutex> lock(clientMux_);
    old_count = client_count_;
    for (int i = 0; i < old_count; i++) {
      removed[i] = connected_clients_[i];
    }
    client_count_ = 0;
    memset(connected_clients_, 0, sizeof(connected_clients_));
  }

  // Логирование и посты — ВНЕ мьютекса.
  if (old_count > 0) {
    ESP_LOGI(TAG, "Cleaned up %d client(s)", old_count);
    // Пара к каждому WS_CLIENT_CONNECTED: без DISCONNECTED счётчики
    // аудитории (J1939Channel) зависли бы >0 после рестарта httpd и
    // снапшоты продолжали бы уходить в пустоту.
    for (int i = 0; i < old_count; i++) {
      ws_message_t msg = {};
      msg.sockfd = removed[i];
      postEvent<app_event_id_t::WS_CLIENT_DISCONNECTED>(ctx_->events, msg);
    }
  }
}

esp_err_t WsHandler::onPostHandshake(httpd_req_t *req) {
  WsHandler *self = static_cast<WsHandler *>(req->user_ctx);
  if (!self || !self->server_) {
    return ESP_FAIL;
  }

  int sockfd = httpd_req_to_sockfd(req);
  ESP_LOGI(TAG, "WebSocket handshake done, registering client sockfd: %d", sockfd);
  self->add_client(sockfd);
  return ESP_OK;
}

esp_err_t WsHandler::ws_handler(httpd_req_t *req) {
  WsHandler *self = static_cast<WsHandler *>(req->user_ctx);
  if (!self || !self->server_) {
    return ESP_FAIL;
  }

  int sockfd = httpd_req_to_sockfd(req);

  if (req->method == HTTP_GET) {
    ESP_LOGI(TAG, "WebSocket handshake, sockfd: %d", sockfd);
#if !CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
    // При поддержке post-handshake callback клиент регистрируется в
    // onPostHandshake(); регистрация здесь привела бы к двойному
    // WS_CLIENT_CONNECTED и двойному PUSH всех полей.
    self->add_client(sockfd);
#endif
    return ESP_OK;
  }

  httpd_ws_frame_t ws_pkt = {};
  esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to receive frame header: %s, sockfd: %d",
             esp_err_to_name(ret), sockfd);
    self->remove_client(sockfd);
    return ret;
  }

  const char* type_str = "UNKNOWN";
  switch (ws_pkt.type) {
    case HTTPD_WS_TYPE_TEXT:    type_str = "TEXT"; break;
    case HTTPD_WS_TYPE_BINARY:  type_str = "BINARY"; break;
    case HTTPD_WS_TYPE_PING:    type_str = "PING"; break;
    case HTTPD_WS_TYPE_PONG:    type_str = "PONG"; break;
    case HTTPD_WS_TYPE_CLOSE:   type_str = "CLOSE"; break;
    case HTTPD_WS_TYPE_CONTINUE: type_str = "CONT"; break;
  }
  ESP_LOGD(TAG, "Received WS frame: type=%s (%d), len=%u, sockfd=%d",
           type_str, (int)ws_pkt.type, ws_pkt.len, sockfd);

  if (ws_pkt.type == HTTPD_WS_TYPE_PING || ws_pkt.type == HTTPD_WS_TYPE_PONG ||
      ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {

    // Сначала считываем payload (если есть)
    // unique_ptr с deleter free: буфер освобождается на любом пути выхода
    // (раньше — ручной free перед каждым return: пропуск = утечка до 125 Б
    // на каждый кадр).
    std::unique_ptr<uint8_t, decltype(&std::free)> payload(nullptr, &std::free);
    if (ws_pkt.len > 0) {
      // Лимит до malloc: RFC 6455 §5.5 ограничивает control frame 125 Б,
      // но снаружи это не проверяется — защита от OOM на огромной
      // declared-длине.
      if (ws_pkt.len > kMaxWsInboundLen) {
        ESP_LOGW(TAG, "Control frame too large: %u, dropping", ws_pkt.len);
        return ESP_ERR_INVALID_SIZE;
      }
      payload.reset(static_cast<uint8_t*>(std::malloc(ws_pkt.len)));
      if (payload) {
        ws_pkt.payload = payload.get();
        esp_err_t err = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (err != ESP_OK) {
          ESP_LOGW(TAG, "Failed to read PING/PONG payload");
          return err;
        }
      }
    }

    // OOM на payload управляющего кадра: нельзя отвечать PONG с ненулевой
    // длиной и payload=nullptr (OOB-чтение при отправке) — отбрасываем кадр.
    if (ws_pkt.len > 0 && !payload) {
      ESP_LOGE(TAG, "OOM reading control frame payload, dropping");
      return ESP_ERR_NO_MEM;
    }

    // Обрабатываем PING → отправляем PONG
    if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
      ESP_LOGD(TAG, "Received PING, sending PONG (len=%d)", ws_pkt.len);
      httpd_ws_frame_t pong = {};
      pong.type = HTTPD_WS_TYPE_PONG;
      pong.len = ws_pkt.len;
      pong.payload = payload.get(); // тот же payload, что и в PING
      esp_err_t err = httpd_ws_send_frame(req, &pong);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send PONG");
      }
    }

    // Обрабатываем CLOSE
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
      ESP_LOGD(TAG, "Received CLOSE frame");
      self->remove_client(sockfd);
    }

    // Освобождение payload — автоматически (unique_ptr) при выходе из блока.
    return ESP_OK;
  }

  if (ws_pkt.type != HTTPD_WS_TYPE_BINARY &&
      ws_pkt.type != HTTPD_WS_TYPE_TEXT) {
    ESP_LOGW(TAG, "Unknown frame type %d", (int)ws_pkt.type);
    return ESP_OK;
  }

  if (ws_pkt.len == 0 || ws_pkt.len > kMaxWsInboundLen) {
    ESP_LOGW(TAG, "Invalid WS message size: %d", ws_pkt.len);
    self->remove_client(sockfd);
    return ESP_ERR_INVALID_SIZE;
  }

  // Rate-limit до malloc/post: флуд TEXT/BINARY не заливает event-loop.
  // Только дроп кадра — соединение не трогаем (control-кадры выше не идут).
  if (!self->allowInboundFrame(sockfd)) {
    const uint32_t n = self->rxDrops_.fetch_add(1, std::memory_order_relaxed) + 1;
    // Rate-limit лога: первый дроп и каждый 50-й.
    if (n == 1 || (n % 50) == 0) {
      ESP_LOGW(TAG, "RX rate limit exceeded, frame dropped (sockfd=%d, total=%u)",
               sockfd, (unsigned)n);
    }
    return ESP_OK;
  }

  // malloc + unique_ptr с deleter free: выравнивание max_align_t под
  // ws_message_t (flexible array) + освобождение на всех путях выхода
  // (раньше — ручной free, один пропущенный = утечка).
  size_t totalSize = sizeof(ws_message_t) + ws_pkt.len;
  std::unique_ptr<ws_message_t, decltype(&std::free)> msg(
      static_cast<ws_message_t*>(std::malloc(totalSize)), &std::free);
  if (!msg) {
    ESP_LOGE(TAG, "Failed to allocate ws_message_t");
    return ESP_ERR_NO_MEM;
  }

  msg->sockfd = sockfd;
  msg->length = ws_pkt.len;

  ws_pkt.payload = (uint8_t *)msg->data;
  ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to receive frame data: %s", esp_err_to_name(ret));
    self->remove_client(sockfd);
    return ret;
  }

  postSizedEvent<app_event_id_t::WS_MESSAGE_RECEIVED>(
      self->ctx_->events, msg.get(), totalSize);
  // ← payload копируется event loop'ом, здесь буфер уже не нужен;
  // освободит unique_ptr (и на раннем return выше — тоже).
  return ESP_OK;
}

// --- Путь отправки: event-loop -> очередь -> sender-задача ---------------

void WsHandler::onWsMessageSend(const ws_message_t *msg) {
  // Колбэк event-loop задачи. НЕ отправляем здесь: httpd_ws_send_frame_async
  // на самом деле синхронный (send с SO_SNDTIMEO=5 с) — медленный/мёртвый
  // клиент блокировал бы шину событий до 5 с на кадр. Вместо этого —
  // своя копия сообщения в очередь sender-задачи (payload события после
  // возврата из колбэка принадлежит event-loop'у — копия обязательна).
  if (!server_ || !msg || !txQueue_) {
    return;
  }
  if (msg->length == 0 || msg->length > kMaxWsMessageLen) {
    ESP_LOGW(TAG, "Bad WS tx length %u, dropping", (unsigned)msg->length);
    txDrops_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  WsTxItem* item = makeTxItem(msg);
  if (!item) {
    txDrops_.fetch_add(1, std::memory_order_relaxed);
    ESP_LOGE(TAG, "OOM for WS tx item (%u B)", (unsigned)msg->length);
    return;
  }

  if (xQueueSend(txQueue_, &item, 0) != pdTRUE) {
    std::free(item);
    uint32_t n = txDrops_.fetch_add(1, std::memory_order_relaxed) + 1;
    // Rate-limit: первый дроп и каждый 10-й дальше.
    if (n == 1 || (n % 10) == 0) {
      ESP_LOGW(TAG, "WS tx queue full, item dropped (total drops=%u)",
               (unsigned)n);
    }
  }
}

WsHandler::WsTxItem* WsHandler::makeTxItem(const ws_message_t* msg) {
  auto* item = static_cast<WsTxItem*>(std::malloc(sizeof(WsTxItem) + msg->length));
  if (!item) {
    return nullptr;
  }
  item->sockfd = msg->sockfd;
  item->len = msg->length;
  std::memcpy(item->data, msg->data, msg->length);
  return item;
}

void WsHandler::senderTrampoline(void* arg) {
  static_cast<WsHandler*>(arg)->senderLoop();
}

void WsHandler::senderLoop() {
  for (;;) {
    WsTxItem* item = nullptr;
    // Таймаут цикла — способ заметить senderStop_, когда очередь пуста:
    // в отличие от DnsServer (закрытие сокета будит recvfrom) очередь
    // будить нечем.
    if (xQueueReceive(txQueue_, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
      if (senderStop_.load(std::memory_order_acquire)) {
        break;
      }
      continue;
    }
    if (!item) {
      continue;
    }
    deliverItem(item);
    std::free(item);
  }
  // Подтверждение стопперу ПЕРЕД self-delete (паттерн DnsServer::stop):
  // stopTx() ждёт семафор, после чего хэндл задачи трогать нельзя.
  if (senderDoneSem_) {
    xSemaphoreGive(senderDoneSem_);
  }
  vTaskDelete(nullptr);
}

void WsHandler::deliverItem(const WsTxItem* item) {
  httpd_handle_t server = server_.load();
  if (!server) {
    return;
  }

  if (item->sockfd == -1) {
    send_to_all_clients(reinterpret_cast<const char*>(item->data), item->len);
    return;
  }

  httpd_ws_frame_t ws_pkt = {};
  ws_pkt.payload = const_cast<uint8_t*>(item->data);
  ws_pkt.len = item->len;
  ws_pkt.type = HTTPD_WS_TYPE_BINARY;
  sendToSock(server, item->sockfd, &ws_pkt);
}

void WsHandler::send_to_all_clients(const char *data, size_t len) {
  httpd_handle_t server = server_.load();
  if (!server) {
    return;
  }

  // 1. Локальная копия списка сокетов под мьютексом...
  int local_clients[kMaxClients];
  int local_count = 0;
  {
    std::lock_guard<std::mutex> lock(clientMux_);
    local_count = client_count_;
    for (int i = 0; i < local_count; i++) {
      local_clients[i] = connected_clients_[i];
    }
  }

  // 2. ...отправка ВНЕ мьютекса (send может блокироваться до SO_SNDTIMEO).
  httpd_ws_frame_t ws_pkt = {};
  ws_pkt.payload = (uint8_t *)data;
  ws_pkt.len = len;
  ws_pkt.type = HTTPD_WS_TYPE_BINARY;

  for (int i = 0; i < local_count; i++) {
    sendToSock(server, local_clients[i], &ws_pkt);
  }
}

void WsHandler::sendToSock(httpd_handle_t server, int sockfd,
                           httpd_ws_frame_t* pkt) {
  if (sockfd <= 0) {
    return;
  }
  esp_err_t ret = httpd_ws_send_frame_async(server, sockfd, pkt);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to send to client %d: %s", sockfd,
             esp_err_to_name(ret));
    // remove_client сам берёт clientMux_ — вызов вне мьютекса.
    remove_client(sockfd);
  } else {
    txSent_.fetch_add(1, std::memory_order_relaxed);
  }
}

// --- Жизненный цикл -------------------------------------------------------

void WsHandler::stopTx() {
  if (!senderTask_) {
    return;
  }
  senderStop_.store(true, std::memory_order_release);

  // Задача даёт семафор перед vTaskDelete(nullptr); таймаут больше
  // SO_SNDTIMEO (5 с), чтобы не удалить задачу внутри send. Сужаем гонку
  // «Give пришёл ровно на границе таймаута» коротким повторным take.
  if (xSemaphoreTake(senderDoneSem_, kSenderStopTimeoutMs / portTICK_PERIOD_MS) == pdTRUE ||
      xSemaphoreTake(senderDoneSem_, 100 / portTICK_PERIOD_MS) == pdTRUE) {
    senderTask_ = nullptr;
  } else {
    // Задача зависла (в send на мёртвом клиенте и т.п.): принудительно.
    ESP_LOGE(TAG, "ws sender did not stop in time, forcing delete");
    vTaskDelete(senderTask_);
    senderTask_ = nullptr;
  }
  // Готовность к повторному запуску в reg().
  senderStop_.store(false, std::memory_order_release);
}

esp_err_t WsHandler::reg(httpd_handle_t server) {
  server_ = server;

  {
    std::lock_guard<std::mutex> lock(clientMux_);
    client_count_ = 0;
    memset(connected_clients_, 0, sizeof(connected_clients_));
  }
  // Рестарт httpd: sockfd переиспользуются — старые окна rate-limit сбрасываем.
  for (auto& s : rxRate_) {
    s = RxRateSlot{};
  }

  // Очередь и sender-задача — до подписки (иначе первые события упадут в
  // отсутствие очереди). Идемпотентно: reg() может повторяться.
  if (!txQueue_) {
    txQueue_ = xQueueCreate(kTxQueueDepth, sizeof(WsTxItem*));
    if (!txQueue_) {
      ESP_LOGE(TAG, "Failed to allocate WS tx queue");
      return ESP_ERR_NO_MEM;
    }
  }
  if (!senderDoneSem_) {
    senderDoneSem_ = xSemaphoreCreateBinary();
    if (!senderDoneSem_) {
      ESP_LOGE(TAG, "Failed to allocate WS sender semaphore");
      return ESP_ERR_NO_MEM;
    }
  }
  if (!senderTask_) {
    senderStop_.store(false, std::memory_order_release);
    if (xTaskCreate(senderTrampoline, "ws_tx", kSenderTaskStack, this,
                    5, &senderTask_) != pdPASS) {
      senderTask_ = nullptr;
      ESP_LOGE(TAG, "Failed to create WS sender task");
      return ESP_ERR_NO_MEM;
    }
  }

  // Единая точка подписки через EventManager. Подписываемся один раз за время
  // жизни объекта: при рестарте httpd (NetworkController) reg() вызывается
  // повторно, а дубли подписок исчерпали бы пул EventManager.
  if (!subs_registered_) {
    if (!subscribeEvent<app_event_id_t::WS_MESSAGE_SEND>(
            ctx_->events, &WsHandler::onWsMessageSend, this)) {
      ESP_LOGE(TAG, "subscribe(WS_MESSAGE_SEND) failed");
      return ESP_FAIL;
    }
    subs_registered_ = true;
  }

  httpd_uri_t ws_uri = {};
  ws_uri.uri = "/ws";
  ws_uri.method = HTTP_GET;
  ws_uri.handler = ws_handler;
  ws_uri.user_ctx = this;
  ws_uri.is_websocket = true;
  ws_uri.handle_ws_control_frames = true;
#if CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
  ws_uri.ws_post_handshake_cb = onPostHandshake;
#endif

  return httpd_register_uri_handler(server, &ws_uri);
}

void WsHandler::unreg() {
  ESP_LOGI(TAG, "Unregistering WebSocket handler (tx sent=%u, drops=%u, rx drops=%u)",
           (unsigned)txSent_.load(), (unsigned)txDrops_.load(),
           (unsigned)rxDrops_.load());
  // Порядок: сначала sender (он держит server_ и может дёргать
  // remove_client), потом снос клиентов, потом server_ = nullptr.
  stopTx();
  cleanup_clients();
  server_ = nullptr;
}
