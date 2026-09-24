#pragma once
#include <cstddef>
#include <cstdint>

// Формат кадра и батча — см. docs/PROTOCOL-J1939.md
namespace J1939Proto {

constexpr uint8_t  kMagic0        = 0x5A;
constexpr uint8_t  kMagic1        = 0xA5;
constexpr uint8_t  kVersion       = 1;
constexpr uint16_t kHeaderSize    = 10;   // magic(2)+ver(1)+flags(1)+msgType(2)+len(2)+seq(2)
constexpr uint16_t kCrcSize       = 2;
constexpr uint16_t kMinPacketSize = kHeaderSize + kCrcSize;
constexpr uint8_t  kFlagSnapshot  = 0x20;
constexpr uint16_t kMsgTypeSnapshot = 0x0001;
constexpr uint16_t kMsgTypeRequest  = 0x0002;
constexpr uint16_t kJ1939MaxDataLen = 1785;  // 255 пакетов TP.DT * 7

// Запись батча (один PGN).
struct BatchRecord {
    uint8_t       sa;
    uint32_t      pgn;   // 24 бита (0..0x3FFFF)
    uint16_t      len;   // 1..1785
    const uint8_t* data;
    uint16_t      periodMs;
};

uint16_t crc16(const uint8_t* data, size_t len);       // CCITT-FALSE, init 0xFFFF
size_t   batchPayloadSize(const BatchRecord* recs, size_t count); // нужный размер
size_t   serializeBatch(uint8_t* out, size_t outCap,
                        const BatchRecord* recs, size_t count);   // 0 = не влезло
size_t   wrapFrame(uint16_t msgType, uint8_t flags,
                   const uint8_t* payload, size_t payloadLen,
                   uint16_t seq, uint8_t* out, size_t outCap);    // 0 = не влезло
bool     unwrapFrame(const uint8_t* frame, size_t len,
                     uint16_t* msgType, uint8_t* flags,
                     const uint8_t** payload, size_t* payloadLen); // + проверка CRC
}