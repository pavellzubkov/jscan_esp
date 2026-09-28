#pragma once
#include "driver/gpio.h"
#include <cstdint>

namespace Hw {
// Версия прошивки — единый источник (дефолт SystemFields.inc + writeFieldScalar).
constexpr const char* kFwVersion = "1.0.0";
// Пины TWAI.
constexpr gpio_num_t kCanTxGpio = GPIO_NUM_5;
constexpr gpio_num_t kCanRxGpio = GPIO_NUM_4;
// Битрейт J1939 по умолчанию (CAN 2.0B, 250 kbps). Актуальное значение —
// конфигурируемое поле canBitrate (AppData), применяется при перезагрузке.
constexpr uint32_t kCanBitrate = 250000;
// PGN служебных сообщений.
constexpr uint32_t kPgnRequest   = 59904;   // RQST
constexpr uint32_t kPgnTpCm      = 60416;   // TP.CM (connection management)
constexpr uint32_t kPgnTpDt      = 60160;   // TP.DT (data transfer)
// Статический IP softAP (10.10.10.10/24). Значения в network byte order —
// так их хранит поле .addr структуры esp_ip4_addr_t (esp_netif_set_ip_info).
constexpr uint32_t kApIp      = 0x0A0A0A0A; // 10.10.10.10
constexpr uint32_t kApGateway = 0x0A0A0A0A; // 10.10.10.10
constexpr uint32_t kApNetmask = 0x00FFFFFF; // 255.255.255.0
}