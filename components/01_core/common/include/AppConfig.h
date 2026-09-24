#pragma once
#include <cstdint>

struct AppConfig {
    // Точка доступа
    char     apSsid[32]     = "J1939_AP";
    char     apPassword[32] = "12345678";
    uint8_t  apChannel      = 6;
    uint8_t  maxStaConn     = 2;
    // Снапшот J1939
    uint32_t snapshotIntervalMs = 250;   // период батч-фрейма (200–500 мс)
    uint32_t snapshotTtlMs       = 2000; // TTL «активности» PGN
    uint16_t maxTrackedPgns      = 128;  // потолок карты аккумулятора
};