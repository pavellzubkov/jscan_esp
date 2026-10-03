#pragma once
#include <cstddef>
#include <cstdint>

// Общие хелперы порядка байт (little-endian). Header-only — не плодит TU.
// Единый источник вместо дублей readUnsignedLE/readSignedLE в FieldRegistry
// и ConfigStore и ручной упаковки uid u16 LE в CommModule.

// Беззнаковое целое n байт, LE (n = 1..8).
inline uint64_t readUnsignedLE(const uint8_t* p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

// Знаковое целое n байт, LE (дополняющий код); n = 1..8.
inline int64_t readSignedLE(const uint8_t* p, size_t n) {
    uint64_t v = readUnsignedLE(p, n);
    if (n < 8) {
        bool neg = v & (uint64_t(1) << (8 * n - 1));
        if (neg) v |= ~((uint64_t(1) << (8 * n)) - 1);
    }
    return static_cast<int64_t>(v);
}

// uint16 LE — uid и другие 16-битные поля протокола.
inline uint16_t readU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline void writeU16LE(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>(v >> 8);
}
