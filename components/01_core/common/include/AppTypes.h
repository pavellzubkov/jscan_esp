#pragma once
#include <cstdint>
#include <cstring>

// === UID поля: вычисляется хэшем от имени (FNV-1a 32-bit, младшие 16 бит) ===
// Используется на прошивке (constexpr) и на клиенте — алгоритм обязан
// совпадать на обеих сторонах.
inline constexpr uint32_t fnv1a32(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) { h ^= static_cast<uint8_t>(*s); h *= 16777619u; }
    return h;
}
inline constexpr uint16_t fieldUid(const char* name) {
    return static_cast<uint16_t>(fnv1a32(name) & 0xFFFFu);
}

// Фиксированные строки вместо std::string.
struct FixedString {
  char data[32];

  constexpr FixedString(const char *str = "") : data{} {
    if (str) {
      for (size_t i = 0; i < sizeof(data) - 1 && str[i] != '\0'; ++i) {
        data[i] = str[i];
      }
    }
  }

  operator const char *() const { return data; }
  const char *c_str() const { return data; }
};

// Домен (владелец/подсистема) поля. Совпадает с файлом-доменом в DataFields.inc
// и используется для контроля владения записью runtime-полей (см. FieldRegistry).
enum class FieldDomain : uint8_t {
  WIFI,
  TWAI,
  SNAPSHOT,
  SYSTEM,
  PROTOCOL   // sentinel: пишущий по протоколу (не владеет ни одним runtime-полем)
};

// === Макросы для меток валидации ===
#define CFG_STRING 0
#define CFG_INT 1
#define CFG_UINT 2
#define CFG_FLOAT 3
#define CFG_ENUM 4
#define CFG_IP 5
#define CFG_BOOL 6
