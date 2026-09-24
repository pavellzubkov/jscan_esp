# STEP-05 — `04_network/wifi` + `04_network/server`

> Статус: **done**

> Контекст: свежая сессия. STEP-01..04 выполнены, проект собирается. Этот шаг
> создаёт Wi-Fi AP-модуль и веб-сервер (HTTP static + WebSocket). Модули пока
> не подключены в main (до STEP-07), но компилируются.
> Проверка: `idf.py reconfigure && idf.py build`.

## Цель

- `04_network/wifi/WifiApModule` — softAP, конфиг из `ctx->config`, публикация
  `WIFI_STATUS`.
- `04_network/server/ServerModule` — httpd + StaticHandler (раздача из SPIFFS) +
  `WsHandler` (порт из TEMP_PID): приём входящих WS-сообщений → `WS_MESSAGE_RECEIVED`,
  отправка по событию `WS_MESSAGE_SEND`, регистрация клиентов.

## Структура

```
components/04_network/wifi/
  CMakeLists.txt
  include/WifiApModule.hpp
  src/WifiApModule.cpp
components/04_network/server/
  CMakeLists.txt
  include/ServerModule.hpp
  src/ServerModule.cpp
  src/StaticHandler.cpp
  src/StaticHandler.hpp
  src/WsHandler.cpp
  src/WsHandler.hpp
```

## 1. `wifi`

### `include/WifiApModule.hpp`

```cpp
#pragma once
#include "AppContext.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include <cstdint>

// SoftAP-точка доступа. Конфиг (ssid/pass/channel/maxStaConn) из ctx->config.
// Публикует WIFI_STATUS на событиях AP_STA*.
class WifiApModule {
public:
    explicit WifiApModule(AppContext* ctx);
    ~WifiApModule();

    esp_err_t begin();   // esp_netif_init + create_default_wifi_ap + start
    void      stop();

private:
    AppContext* ctx_;
    esp_netif_t* netif_ = nullptr;
    bool started_ = false;

    static void eventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
    void onEvent(esp_event_base_t base, int32_t id, void* data);
};
```

### `src/WifiApModule.cpp` — порт `esp_wifi_start/esp_wifi_start.cpp`

- `begin()`:
  - `esp_netif_init()` (идемпотентно — безопасно вызывать повторно);
  - `esp_wifi_init(WIFI_INIT_CONFIG_DEFAULT())`;
  - `esp_wifi_set_ps(WIFI_PS_NONE)`;
  - регистрация `eventHandler` на default loop (`esp_event_handler_instance_register`)
    для `WIFI_EVENT` (AP_STACONNECTED/AP_STADISCONNECTED);
  - `netif_ = esp_netif_create_default_wifi_ap()`;
  - статический IP из констант (см. `HardwareConfig.h` — добавить
    `kApIp`, `kApGateway`, `kApNetmask`; адрес 10.10.10.10, маска 255.255.255.0);
    `esp_netif_dhcps_stop` → `esp_netif_set_ip_info` → `esp_netif_dhcps_start`;
  - собрать `wifi_config_t` из `ctx->config.apSsid/apPassword/apChannel/maxStaConn`;
  - `esp_wifi_set_mode(WIFI_MODE_AP)` → `esp_wifi_set_config(WIFI_IF_AP, &cfg)` →
    `esp_wifi_start()`.
- `onEvent`: на `WIFI_EVENT_AP_STACONNECTED/DISCONNECTED` — пересчитать число
  клиентов (`esp_wifi_ap_get_sta_list` + `esp_wifi_ap_get_sta_num`) и
  `ctx_->events.post(WIFI_STATUS, {is_ap_mode:true, num_clients,...})`.

### `CMakeLists.txt`

```cmake
idf_component_register(
    SRC_DIRS .
    INCLUDE_DIRS .
    REQUIRES common esp_wifi esp_netif esp_event freertos log
)
```

## 2. `server`

### `include/ServerModule.hpp`

```cpp
#pragma once
#include "esp_http_server.h"
#include "AppContext.h"
#include "spiffs_service/SpiffsService.hpp"

class WsHandler;   // fwd, чтобы не тянуть WsHandler.hpp в include/

class ServerModule {
public:
    explicit ServerModule(AppContext* ctx);
    ~ServerModule();

    esp_err_t begin();   // смонтировать ФС + старт httpd + static + ws
    void      stop();

    httpd_handle_t getHandle() const { return server_; }

private:
    AppContext* ctx_;
    httpd_handle_t server_ = nullptr;
    WsHandler* ws_ = nullptr;
};
```

### `src/WsHandler.hpp/.cpp` — **порт из TEMP_PID**

Источник: `E:\Projects\Embedded\ESP32\Temp_pid\TEMP_PID\components\04_network\server\src\WsHandler.hpp/.cpp`
(взять почти как есть, адаптировать include-пути):
- реестр клиентов `connected_clients_[10]`, `add_client`/`remove_client`/
  `cleanup_clients` под `portMUX_TYPE` (всё логирование ВНЕ критической секции);
- `reg(httpd_handle_t server)` — регистрирует `/ws`, подписка на
  `WS_MESSAGE_SEND` через EventManager один раз (`subs_registered_`);
- `onPostHandshake` (IDF ≥5.5: `ws_post_handshake_cb`) → `add_client` +
  `post WS_CLIENT_CONNECTED`;
- `ws_handler`:
  - HTTP_GET → (без add_client при наличии post-handshake);
  - управляющие кадры (PING/PONG/CLOSE) — обрабатывать, на PING отвечать PONG;
  - иначе: кламп длины `<= kMaxWsMessageLen` (8192), malloc `ws_message_t+len`,
    `httpd_ws_recv_frame`, `postSized(WS_MESSAGE_RECEIVED)`, `free`;
- `onWsMessageSend(const ws_message_t*)`: `sockfd == -1` → broadcast всем,
  иначе адресно `httpd_ws_send_frame_async`; при ошибке — `remove_client`.
  ВАЖНО: `httpd_ws_send_frame_async` копирует payload во внутренний буфер — можно
  освобождать после post.

### `src/StaticHandler.cpp/.hpp` — порт `_fs_server.cpp`

- `reg_static_handler(httpd_handle_t server)` — глобальный GET `/*` с
  `httpd_uri_match_wildcard` (как `start_http` + `start_rest_server` в старом коде);
- хендлер: `filepath = base("/spiffs") + uri`, поддержка `index.html` для `/`,
  gzip `.gz` fallback, `set_content_type_from_file` (копировать таблицу типов);
  читать чанками `SCRATCH_BUFSIZE` (взять из старого кода).
- Добавить `Access-Control-Allow-Origin: *` (как в старом коде).

### `src/ServerModule.cpp`

- `begin()`:
  - `SpiffsService fs_("/spiffs","storage",false); fs_.mount();` (идемпотентно —
    конфиг уже смонтировал в ConfigStore::begin);
  - `httpd_config_t config = HTTPD_DEFAULT_CONFIG(); config.lru_purge_enable=true;
    config.uri_match_fn = httpd_uri_match_wildcard;`
  - `httpd_start(&server_, &config)`;
  - `reg_static_handler(server_)`;
  - `ws_ = new WsHandler(ctx_); ws_->reg(server_);`
  - на `stop()`: `ws_->unreg()`, `httpd_stop(server_)`, delete ws_.
- Если httpd не стартует (например, занят порт) — логировать, возвращать ошибку
  (NetCtrl в STEP-07 решит, критично или нет).

### `CMakeLists.txt`

```cmake
idf_component_register(
    SRC_DIRS .
    INCLUDE_DIRS .
    REQUIRES common spiffs_service esp_http_server esp_wifi esp_netif freertos log
)
```

## Критерий готовности

1. `idf.py reconfigure && idf.py build` — успех.
2. Компоненты компилируются; в STEP-07 после подключения проверим:
   - AP `J1939_AP` поднимается, браузер открывает `http://10.10.10.10`
     (отдаётся `spiffs_image/index.html`);
   - WS `/ws` подключается, в логе виден `WS_CLIENT_CONNECTED`.

## Замечания

- `WsHandler` не должен «прослушивать» сырые `J1939Msg*` из очередей — все данные
  идут через события `WS_MESSAGE_SEND` (это и есть суть рефакторинга).
- `esp_netif_init()` вызывается в WifiApModule; ServerModule его не дублирует.
- Не удалять старые `esp_wifi_start`/`esp_fs_start`.