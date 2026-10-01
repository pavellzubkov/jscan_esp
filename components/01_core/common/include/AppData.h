#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include "AppTypes.h"
#include "HardwareConfig.h"
#include "SystemTiming.h"   // дефолты SnapshotFields.inc (Timing::kSnapshot*)

// ================================================================
// Реестр полей системы: доменные .inc-файлы включены явно, чтобы
// избежать директив препроцессора внутри макросов (UB).
//
// Домены (порядок определяет порядок сер/порядок config-blob,
// на UID не влияет — UID = fnv1a32(имя)&0xFFFF) :
//   WifiFields.inc     → FieldDomain::WIFI
//   TwaiFields.inc     → FieldDomain::TWAI
//   SnapshotFields.inc → FieldDomain::SNAPSHOT
//   SystemFields.inc   → FieldDomain::SYSTEM
// ================================================================

// ---------------------------------------------------------------
// Структура AppData (плоская сериализуемая схема)
// ---------------------------------------------------------------
#undef DATA_FIELD
#define DATA_FIELD(type, name, def, min, max, validator, isConfig, readonly) type name;

struct AppData {
#include "WifiFields.inc"
#include "TwaiFields.inc"
#include "SnapshotFields.inc"
#include "SystemFields.inc"
};

#undef DATA_FIELD

// ---------------------------------------------------------------
// Инициализация по умолчанию
// ---------------------------------------------------------------
inline void initAppDataDefault(AppData& data) {
#undef DATA_FIELD
#define DATA_FIELD(type, name, def, min, max, validator, isConfig, readonly) \
    data.name = (def);

#include "WifiFields.inc"
#include "TwaiFields.inc"
#include "SnapshotFields.inc"
#include "SystemFields.inc"

#undef DATA_FIELD
}

// ---------------------------------------------------------------
// Метаданные
// ---------------------------------------------------------------
struct FieldMeta {
    uint16_t uid;        // UID поля (fnv1a32(имя)&0xFFFF)
    const char* name;
    size_t offset;
    size_t size;
    bool isConfig;
    bool readonly;
    uint8_t  validator;  // CFG_STRING / CFG_INT / CFG_UINT / CFG_FLOAT / CFG_ENUM / CFG_IP / CFG_BOOL / CFG_PASSWORD
    double   minVal;
    double   maxVal;
    FieldDomain domain;  // домен/владелец поля (для контроля владения записи)
};

// 0xFFFF зарезервирован (не используется как UID поля) — защита от коллизий.
constexpr uint16_t FULL_ID = 0xFFFF; // резерв: не является валидным UID поля

#undef FIELD_DOMAIN
#undef DATA_FIELD
#define DATA_FIELD(type, name, def, min, max, validator, isConfig, readonly) \
    { \
        fieldUid(#name), \
        #name, \
        offsetof(AppData, name), \
        sizeof(type), \
        static_cast<bool>(isConfig), \
        static_cast<bool>(readonly), \
        (validator), \
        static_cast<double>(min), \
        static_cast<double>(max), \
        FIELD_DOMAIN \
    }, \

constexpr inline FieldMeta g_fieldMeta[] = {
#define FIELD_DOMAIN FieldDomain::WIFI
#include "WifiFields.inc"
#undef FIELD_DOMAIN
#define FIELD_DOMAIN FieldDomain::TWAI
#include "TwaiFields.inc"
#undef FIELD_DOMAIN
#define FIELD_DOMAIN FieldDomain::SNAPSHOT
#include "SnapshotFields.inc"
#undef FIELD_DOMAIN
#define FIELD_DOMAIN FieldDomain::SYSTEM
#include "SystemFields.inc"
#undef FIELD_DOMAIN
};
#undef DATA_FIELD

// NOLINTNEXTLINE(clang-diagnostic-sizeof-array-div)
constexpr size_t kAppFieldCount = sizeof(g_fieldMeta) / sizeof(g_fieldMeta[0]);

// Проверка уникальности вычисленных UID и резерва FULL_ID (0xFFFF).
// Срабатывает на этапе компиляции при любой коллизии хэша или совпадении с FULL_ID.
constexpr bool checkFieldUids() {
    for (size_t i = 0; i < kAppFieldCount; ++i) {
        if (g_fieldMeta[i].uid == 0xFFFFu) return false;               // резерв FULL_ID
        for (size_t j = i + 1; j < kAppFieldCount; ++j) {
            if (g_fieldMeta[i].uid == g_fieldMeta[j].uid) return false; // коллизия
        }
    }
    return true;
}
static_assert(checkFieldUids(), "UID collision or UID==FULL_ID(0xFFFF) in DataFields.inc");

// ---------------------------------------------------------------
// Размерные константы, выводимые из схемы (DataFields.inc).
// Автоматически пересчитываются при добавлении/изменении полей,
// поэтому не нужно вручную поддерживать магические числа в модулях
// (в частности, в CommunicationModule для буфера пакетов).
// ---------------------------------------------------------------

// Максимальный размер сериализованного представления одного поля.
// Для строк сериализация = 1 (длина) + содержимое; за счёт того, что
// FixedString уже содержит весь буфер внутри struct, это не превышает
// meta->size — поэтому оценка через max(meta->size) корректна.
constexpr size_t computeMaxFieldSize() {
    size_t max = 0;
    for (size_t i = 0; i < kAppFieldCount; ++i) {
        if (g_fieldMeta[i].size > max) max = g_fieldMeta[i].size;
    }
    return max;
}
inline constexpr size_t kAppMaxFieldSize = computeMaxFieldSize();

// Максимальный суммарный размер сериализованного тела AppData (без
// заголовка и CRC). = сумма meta->size по всем полям, что является
// гарантированной верхней границей фактической сериализации.
constexpr size_t computeSerializeDataMax() {
    size_t total = 0;
    for (size_t i = 0; i < kAppFieldCount; ++i) {
        total += g_fieldMeta[i].size;
    }
    return total;
}
inline constexpr size_t kAppSerializeDataMax = computeSerializeDataMax();

// ---------------------------------------------------------------
// UID-константы для полей (используются в событиях)
// ---------------------------------------------------------------
#undef DATA_FIELD
#define DATA_FIELD(type, name, def, min, max, validator, isConfig, readonly) \
    constexpr uint16_t name##_UID = fieldUid(#name);

#include "WifiFields.inc"
#include "TwaiFields.inc"
#include "SnapshotFields.inc"
#include "SystemFields.inc"

#undef DATA_FIELD

// ---------------------------------------------------------------
// Вспомогательные функции
// ---------------------------------------------------------------
inline const FieldMeta* findField(const char* name) {
    if (!name) return nullptr;
    for (size_t i = 0; i < kAppFieldCount; ++i) {
        if (strcmp(g_fieldMeta[i].name, name) == 0) {
            return &g_fieldMeta[i];
        }
    }
    return nullptr;
}

inline void* getFieldPtr(AppData& data, const char* name) {
    const FieldMeta* meta = findField(name);
    if (!meta) return nullptr;
    return reinterpret_cast<uint8_t*>(&data) + meta->offset;
}

// ---------------------------------------------------------------
// Type-safe getter (опционально, для удобства)
// ---------------------------------------------------------------
template<typename T>
inline T* getFieldAs(AppData& data, const char* name) {
    const FieldMeta* meta = findField(name);
    if (!meta || meta->size != sizeof(T)) return nullptr;
    return static_cast<T*>(getFieldPtr(data, name));
}

template<typename T>
inline const T* getFieldAs(const AppData& data, const char* name) {
    const FieldMeta* meta = findField(name);
    if (!meta || meta->size != sizeof(T)) return nullptr;
    return static_cast<const T*>(
        reinterpret_cast<const uint8_t*>(&data) + meta->offset
    );
}
