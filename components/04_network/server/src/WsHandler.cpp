#include "WsHandler.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <cstring>
#include <cstdlib>

static const char *TAG = "WsHandler";
static portMUX_TYPE ws_mux = portMUX_INITIALIZER_UNLOCKED;

WsHandler::WsHandler(AppContext* ctx)
    : ctx_(ctx), server_(nullptr), client_count_(0)
{
    memset(connected_clients_, 0, sizeof(connected_clients_));
}

void WsHandler::add_client(int sockfd) {
  bool already_connected = false;
  int old_index = -1;

  taskENTER_CRITICAL(&ws_mux);

  // Проверяем, есть ли уже такой клиент
  for (int i = 0; i < client_count_; i++) {
    if (connected_clients_[i] == sockfd) {
      already_connected = true;
      old_index = i;
      break;
    }
  }

  if (already_connected) {
    // Удаляем старую запись
    for (int j = old_index; j < client_count_ - 1; j++) {
      connected_clients_[j] = connected_clients_[j + 1];
    }
    client_count_--;
  }

  if (client_count_ >= 10) {
    int rejected = sockfd;
    taskEXIT_CRITICAL(&ws_mux);
    // Логирование — ВНЕ критической секции!
    ESP_LOGW(TAG, "Max clients reached, rejecting sockfd: %d", rejected);
    // Закрываем сокет, чтобы клиент не висел подключённым без доставки данных.
    if (server_) {
      httpd_sess_trigger_close(server_, rejected);
    }
    return;
  }

  connected_clients_[client_count_++] = sockfd;
  int new_count = client_count_; // сохраним для лога
  taskEXIT_CRITICAL(&ws_mux);

  // ВСЁ логирование — только здесь, вне критической секции
  if (already_connected) {
    ESP_LOGW(TAG, "Client %d already connected — replaced", sockfd);
  }
  ESP_LOGI(TAG, "Client %d added, total: %d (free heap: %u B)", sockfd,
           new_count, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));

  ws_message_t msg = {};
  msg.sockfd = sockfd;
  ctx_->events.post(APP_EVENTS_BASE, app_event_id_t::WS_CLIENT_CONNECTED, msg);
}

void WsHandler::remove_client(int sockfd) {
  bool removed = false;

  taskENTER_CRITICAL(&ws_mux);
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
  taskEXIT_CRITICAL(&ws_mux);

  if (removed) {
    ESP_LOGI(TAG, "Client %d removed, total: %d (free heap: %u B)", sockfd,
             client_count_, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    ws_message_t msg = {};
    msg.sockfd = sockfd;
    ctx_->events.post(APP_EVENTS_BASE, app_event_id_t::WS_CLIENT_DISCONNECTED, msg);
  }
}

void WsHandler::cleanup_clients() {
  taskENTER_CRITICAL(&ws_mux);
  int old_count = client_count_;
  client_count_ = 0;
  memset(connected_clients_, 0, sizeof(connected_clients_));
  taskEXIT_CRITICAL(&ws_mux);

  // Логирование — ВНЕ критической секции.
  if (old_count > 0) {
    ESP_LOGI(TAG, "Cleaned up %d client(s)", old_count);
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
    uint8_t *payload = nullptr;
    if (ws_pkt.len > 0) {
      payload = (uint8_t *)malloc(ws_pkt.len);
      if (payload) {
        ws_pkt.payload = payload;
        esp_err_t err = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (err != ESP_OK) {
          ESP_LOGW(TAG, "Failed to read PING/PONG payload");
          free(payload);
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
      pong.payload = payload; // тот же payload, что и в PING
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

    // Считывается на каждом PING/PONG/CLOSE-фрейме: освобождаем, чтобы не
    // утекало до 125 байт на каждый кадр.
    if (payload) {
      free(payload);
    }

    return ESP_OK;
  }

  if (ws_pkt.type != HTTPD_WS_TYPE_BINARY &&
      ws_pkt.type != HTTPD_WS_TYPE_TEXT) {
    ESP_LOGW(TAG, "Unknown frame type %d", (int)ws_pkt.type);
    return ESP_OK;
  }

  if (ws_pkt.len == 0 || ws_pkt.len > kMaxWsMessageLen) {
    ESP_LOGW(TAG, "Invalid WS message size: %d", ws_pkt.len);
    self->remove_client(sockfd);
    return ESP_ERR_INVALID_SIZE;
  }

  size_t totalSize = sizeof(ws_message_t) + ws_pkt.len;
  ws_message_t *msg = (ws_message_t *)malloc(totalSize);
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
    free(msg);
    self->remove_client(sockfd);
    return ret;
  }

  self->ctx_->events.postSized(APP_EVENTS_BASE, app_event_id_t::WS_MESSAGE_RECEIVED,
                               msg, totalSize);
  free(msg); // ← payload копируется event loop'ом, поэтому здесь можно освобождать
  return ESP_OK;
}

void WsHandler::onWsMessageSend(const ws_message_t *msg) {
  if (!server_ || !msg) {
    return;
  }

  if (msg->sockfd == -1) {
    send_to_all_clients(msg->data, msg->length);
    return;
  }

  httpd_ws_frame_t ws_pkt = {};
  ws_pkt.payload = (uint8_t *)msg->data;
  ws_pkt.len = msg->length;
  ws_pkt.type = HTTPD_WS_TYPE_BINARY;

  esp_err_t ret = httpd_ws_send_frame_async(server_, msg->sockfd, &ws_pkt);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to send to client %d: %s", msg->sockfd,
             esp_err_to_name(ret));
    remove_client(msg->sockfd);
  }
}

void WsHandler::send_to_all_clients(const char *data, size_t len) {
  if (!server_) {
    return;
  }

  // 1. Делаем локальную копию списка сокетов — БЕЗ критической секции во время
  // отправки
  int local_clients[10];
  int local_count = 0;

  taskENTER_CRITICAL(&ws_mux);
  local_count = client_count_;
  for (int i = 0; i < local_count; i++) {
    local_clients[i] = connected_clients_[i];
  }
  taskEXIT_CRITICAL(&ws_mux);

  // 2. Отправляем ВНЕ критической секции
  httpd_ws_frame_t ws_pkt = {};
  ws_pkt.payload = (uint8_t *)data;
  ws_pkt.len = len;
  ws_pkt.type = HTTPD_WS_TYPE_BINARY;

  for (int i = 0; i < local_count; i++) {
    int sockfd = local_clients[i];
    if (sockfd <= 0)
      continue;

    esp_err_t ret = httpd_ws_send_frame_async(server_, sockfd, &ws_pkt);
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "Failed to send to client %d: %s", sockfd,
               esp_err_to_name(ret));
      // Удаляем клиента — функция remove_client() сама защищена мьютексом
      remove_client(sockfd);
    }
  }
}

esp_err_t WsHandler::reg(httpd_handle_t server) {
  server_ = server;

  client_count_ = 0;
  memset(connected_clients_, 0, sizeof(connected_clients_));

  // Единая точка подписки через EventManager. Подписываемся один раз за время
  // жизни объекта: при рестарте httpd (NetworkController) reg() вызывается
  // повторно, а дубли подписок исчерпали бы пул EventManager.
  if (!subs_registered_) {
    ctx_->events.subscribe(APP_EVENTS_BASE, app_event_id_t::WS_MESSAGE_SEND,
                           &WsHandler::onWsMessageSend, this);
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
  ESP_LOGI(TAG, "Unregistering WebSocket handler");
  cleanup_clients();
  server_ = nullptr;
}