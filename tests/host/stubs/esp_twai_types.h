#pragma once
// Заглушка esp_twai_types.h для host-тестов: TwaiDriver.h (через
// J1939Decoder.h) требует типы узла/кадра TWAI, но .cpp драйвера в тесты
// не входит — достаточно объявлений типов. esp_err.h включаем так же,
// как это делает настоящий esp_twai_types.h (esp_err_t в API драйвера).
#include <cstdint>
#include "esp_err.h"

#define TWAI_FRAME_MAX_LEN 8

struct twai_node_base;                        // opaque (указатель в TwaiDriver)
typedef struct twai_node_base* twai_node_handle_t;

struct twai_rx_done_event_data_t;             // opaque (только указатель)
struct twai_state_change_event_data_t;        // opaque (только указатель;
                                              // TwaiDriver::stateChangeCb, шаг 2)

// Кадр драйвера (по мотивам IDF: TwaiDriver::TxBlock хранит по значению).
typedef struct {
    uint32_t        flags;
    uint32_t        id;
    uint8_t         dlc;
    const uint8_t*  data;
} twai_frame_t;