# STEP-06 — `04_network/communication`

> Статус: **done**

> Контекст: свежая сессия. STEP-01..05 выполнены, проект собирается. Этот шаг
> создаёт CommunicationModule — протокольный слой между WS-транспортом
> (ServerModule/WsHandler) и J1939-системой. Модуль не подключён в main до
> STEP-07, но компилируется. Проверка: `idf.py reconfigure && idf.py build`.

## Цель

`CommunicationModule`:
- подписан на `WS_MESSAGE_RECEIVED` — разбирает входящие кадры
  (`J1939Proto::unwrapFrame`), диспатчит по `MsgType`:
  - `J1939_REQUEST` → `post J1939_REQUEST` (данные `j1939_request_t`);
  - иное → лог + игнор;
- подписан на `J1939_SNAPSHOT_SEND` — байты батча оборачивает в кадр
  (`J1939Proto::wrapFrame`, `MsgType=J1939_SNAPSHOT`, `flags=SNAPSHOT`) и шлёт
  `post WS_MESSAGE_SEND` (broadcast);
- подписан на `WS_CLIENT_CONNECTED/DISCONNECTED` и `WIFI_STATUS` — логирование.

## Структура

```
components/04_network/communication/
  CMakeLists.txt
  include/CommModule.h
  src/CommModule.cpp
```

## `include/CommModule.h`

```cpp
#pragma once
#include "AppContext.h"
#include "AppEvents.h"
#include <cstdint>

// Протокольный слой: кадр (magic/ver/flags/MsgType/len/seq/CRC), диспатч команд,
// обёртка батча снапшота в кадр. Транспорт (WS) — через события WS_MESSAGE_*.
// Без отдельной задачи: обработчики лёгкие (копии ~8 КБ).
class CommunicationModule {
public:
    explicit CommunicationModule(AppContext* ctx);

    esp_err_t begin();

private:
    AppContext* ctx_;
    uint16_t tx_seq_ = 0;   // монотонный счётчик исходящих кадров

    void onIncomingPacket(const ws_message_t* msg);
    void onSnapshot(const j1939_snapshot_t* snap);
    void onWsClientConnected(const ws_message_t* msg);
    void onWsClientDisconnected(const ws_message_t* msg);
    void onWifiStatus(const wifi_status_event_t* s);
};
```

## `src/CommModule.cpp`

### `begin()`

```cpp
esp_err_t CommunicationModule::begin() {
    ctx_->events.subscribe(APP_EVENTS_BASE, app_event_id_t::WS_MESSAGE_RECEIVED,
                           &CommunicationModule::onIncomingPacket, this);
    ctx_->events.subscribe(APP_EVENTS_BASE, app_event_id_t::J1939_SNAPSHOT_SEND,
                           &CommunicationModule::onSnapshot, this);
    ctx_->events.subscribe(APP_EVENTS_BASE, app_event_id_t::WS_CLIENT_CONNECTED,
                           &CommunicationModule::onWsClientConnected, this);
    ctx_->events.subscribe(APP_EVENTS_BASE, app_event_id_t::WS_CLIENT_DISCONNECTED,
                           &CommunicationModule::onWsClientDisconnected, this);
    ctx_->events.subscribe(APP_EVENTS_BASE, app_event_id_t::WIFI_STATUS,
                           &CommunicationModule::onWifiStatus, this);
    ESP_LOGI("Comm", "CommunicationModule ready");
    return ESP_OK;
}
```

### `onIncomingPacket`

- `if (!msg || msg->length == 0) return;`
- `J1939Proto::unwrapFrame((uint8_t*)msg->data, msg->length, &msgType, &flags,
  &payload, &payloadLen)` — если false (magic/ver/CRC) → лог + return;
- `switch (msgType)`:
  - `kMsgTypeRequest`: проверить `payloadLen >= 5`
    (`dstAddr = payload[0]`, `pgn = payload[1] | payload[2]<<8 | payload[3]<<16`),
    `post J1939_REQUEST { dstAddr, pgn }` (данные по значению);
  - default: `ESP_LOGW` «unknown MsgType» (без паники).

### `onSnapshot`

- `if (!snap || snap->length == 0) return;`
- размер кадра: `len = J1939Proto::kHeaderSize + snap->length + J1939Proto::kCrcSize`;
- `uint8_t* frame = (uint8_t*)malloc(len);`
- `J1939Proto::wrapFrame(kMsgTypeSnapshot, kFlagSnapshot, snap->data, snap->length,
  tx_seq_++, frame, len);`
- собрать `ws_message_t` (malloc `sizeof(ws_message_t)+len`), `sockfd=-1`,
  `postSized(WS_MESSAGE_SEND, msg, sizeof+len)`; `free(frame); free(msg);`
- лог на DEBUG: `snap->length` байт, `n записей = snap->data[0]`.

### Логирование подключений/статуса

- `onWsClientConnected/Disconnected`: `ESP_LOGI("Comm","WS client %d connected/disconnected", sockfd);`
- `onWifiStatus`: `ESP_LOGI("Comm","WIFI ap=%d clients=%u", is_ap_mode, num_clients);`

## `CMakeLists.txt`

```cmake
idf_component_register(
    SRC_DIRS .
    INCLUDE_DIRS .
    REQUIRES common freertos log
)
```

(Нужен только `common` — вся логика протокола в `J1939Proto`.)

## Критерий готовности

1. `idf.py reconfigure && idf.py build` — успех.
2. Компонент компилируется; после STEP-07 в мониторе на подключённом WS-клиенте
   видно логирование `WS client ... connected` и периодические кадры снапшота.

## Замечания

- В этом шаге **не** переносим логику `_ws_server.cpp` (старый WS-сервер жил в
  `esp_fs_start` и читал очереди J1939 — он удаляется в STEP-07 вместе со старыми
  компонентами).
- `kMaxWsMessageLen = 8192` достаточно: батч ≤ 8 КБ + кадр ≤ 8192 + 12.
  Если позже понадобится больше — поднять в AppEvents.h и проверить
  `httpd_ws_send_frame_async` (копирует payload).
- Отдельная comm-задача не нужна (обработчики короткие); если event loop начнёт
  «подвисать» на больших батчах — вернуть выделенную задачу как в TEMP_PID.