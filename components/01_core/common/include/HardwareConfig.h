#pragma once
#include "driver/gpio.h"
#include <cstdint>

namespace Hw {
// Адрес узла в J1939-сети.
constexpr uint8_t  kJ1939MyAddr = 25;
// Пины TWAI.
constexpr gpio_num_t kCanTxGpio = GPIO_NUM_5;
constexpr gpio_num_t kCanRxGpio = GPIO_NUM_4;
// Битрейт J1939 (CAN 2.0B, 250 kbps).
constexpr uint32_t kCanBitrate = 250000;
// PGN служебных сообщений.
constexpr uint32_t kPgnRequest   = 59904;   // RQST
constexpr uint32_t kPgnTpCm      = 60416;   // TP.CM (connection management)
constexpr uint32_t kPgnTpDt      = 60160;   // TP.DT (data transfer)
}