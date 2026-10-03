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
// Набор допустимых битрейтов TWAI (поле canBitrate): единственный источник
// списка — валидация конфига в J1939System и членство CFG_ENUM в
// FieldRegistry (kEnumLists), а не дубли в каждом call site.
constexpr uint32_t kAllowedBitrates[] = {125000, 250000, 500000, 1000000};
constexpr bool isValidBitrate(uint32_t br)
{
    for (uint32_t b : kAllowedBitrates)
        if (b == br) return true;
    return false;
}
// Дефолты конфигурируемых TWAI-полей (TwaiFields.inc) и фолбэки чтения
// из реестра (J1939System) — единый источник литералов 25 / 100.
constexpr uint8_t  kDefaultNodeAddr    = 25;
constexpr uint16_t kDefaultTxTimeoutMs = 100;
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