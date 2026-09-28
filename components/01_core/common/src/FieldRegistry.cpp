#include "FieldRegistry.h"
#include "AppTypes.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "FieldRegistry";

// ============================================================
// Сериализация / десериализация значений полей (по PROTOCOL §2)
// (вынесено из CommunicationModule — единый источник для comm/ConfigStore)
// ============================================================

// Размер сериализованного представления одного поля (без padding; строки — с префиксом длины).
static size_t serializedFieldSize(const FieldMeta* meta, const AppData* data) {
    if (meta->validator == CFG_STRING || meta->validator == CFG_IP) {
        const FixedString* fs = reinterpret_cast<const FixedString*>(
            reinterpret_cast<const uint8_t*>(data) + meta->offset);
        size_t sl = strnlen(fs->data, sizeof(fs->data));
        if (sl > 255) sl = 255;
        return 1 + sl;
    }
    return meta->size;
}

static size_t serializeField(const FieldMeta* meta, const AppData* data, uint8_t* out) {
    const uint8_t* src = reinterpret_cast<const uint8_t*>(data) + meta->offset;
    if (meta->validator == CFG_STRING || meta->validator == CFG_IP) {
        const FixedString* fs = reinterpret_cast<const FixedString*>(src);
        size_t sl = strnlen(fs->data, sizeof(fs->data));
        if (sl > 255) sl = 255;
        out[0] = static_cast<uint8_t>(sl);
        memcpy(out + 1, fs->data, sl);
        return 1 + sl;
    }
    memcpy(out, src, meta->size);
    return meta->size;
}

static uint64_t readUnsignedLE(const uint8_t* p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static int64_t readSignedLE(const uint8_t* p, size_t n) {
    uint64_t v = readUnsignedLE(p, n);
    if (n < 8) {
        bool neg = v & (uint64_t(1) << (8 * n - 1));
        if (neg) v |= ~((uint64_t(1) << (8 * n)) - 1);
    }
    return static_cast<int64_t>(v);
}

static bool valueInRange(const FieldMeta* meta, const uint8_t* payload, size_t payloadLen,
                         double minVal, double maxVal) {
    if (meta->validator == CFG_STRING || meta->validator == CFG_IP) {
        if (payloadLen < 1) return false;
        uint8_t sl = payload[0];
        if (sl != payloadLen - 1) return false;          // длина не совпадает
        return (sl >= minVal) && (sl <= maxVal);
    }
    if (meta->validator == CFG_FLOAT) {
        if (payloadLen < sizeof(float)) return false;
        float f; memcpy(&f, payload, sizeof(float));
        double v = f;
        return (v >= minVal) && (v <= maxVal);
    }
    // Целые читаем как знаковые (допускают отрицательные значения);
    // enum / bool — как беззнаковые (0..N).
    if (payloadLen < meta->size) return false;
    double v = (meta->validator == CFG_INT)
                   ? static_cast<double>(readSignedLE(payload, meta->size))
                   : static_cast<double>(readUnsignedLE(payload, meta->size));
    return (v >= minVal) && (v <= maxVal);
}

static bool deserializeField(const FieldMeta* meta, AppData* data,
                             const uint8_t* payload, size_t payloadLen) {
    uint8_t* dst = reinterpret_cast<uint8_t*>(data) + meta->offset;
    if (meta->validator == CFG_STRING || meta->validator == CFG_IP) {
        if (payloadLen < 1) return false;
        uint8_t sl = payload[0];
        if (sl != payloadLen - 1) return false;
        FixedString* fs = reinterpret_cast<FixedString*>(dst);
        size_t cap = sizeof(fs->data);
        size_t n = sl < cap ? sl : cap - 1;
        memcpy(fs->data, payload + 1, n);
        fs->data[n] = '\0';
        return true;
    }
    if (payloadLen < meta->size) return false;
    memcpy(dst, payload, meta->size);
    return true;
}

// ============================================================
// FieldRegistry
// ============================================================

FieldRegistry::FieldRegistry(AppData& data) : data_(data) {}

const FieldMeta* FieldRegistry::getMetaByUid(uint16_t uid) const {
    if (uid == FULL_ID) return nullptr;  // 0xFFFF зарезервирован — не валидный UID поля
    for (size_t i = 0; i < kAppFieldCount; ++i) {
        if (g_fieldMeta[i].uid == uid) return &g_fieldMeta[i];
    }
    return nullptr;
}

const FieldMeta* FieldRegistry::getMetaByName(const char* name) const {
    if (!name) return nullptr;
    for (size_t i = 0; i < kAppFieldCount; ++i) {
        if (strcmp(g_fieldMeta[i].name, name) == 0) return &g_fieldMeta[i];
    }
    return nullptr;
}

size_t FieldRegistry::fieldCount() const {
    return kAppFieldCount;
}

const FieldMeta& FieldRegistry::fieldAt(size_t index) const {
    return g_fieldMeta[index];
}

bool FieldRegistry::readField(uint16_t uid, void* out, size_t out_cap,
                              size_t* out_len) const {
    const FieldMeta* meta = getMetaByUid(uid);
    if (!meta) return false;

    size_t len = serializedFieldSize(meta, &data_);
    if (out_len) *out_len = len;
    if (!out) return true;   // запрос размера
    if (len > out_cap) return false;

    serializeField(meta, &data_, static_cast<uint8_t*>(out));
    return true;
}

FieldWriteStatus FieldRegistry::writeField(uint16_t uid, const void* value,
                                            size_t len, FieldDomain owner) {
    const FieldMeta* meta = getMetaByUid(uid);
    if (!meta) return FieldWriteStatus::UNKNOWN_UID;

    // Контроль владения: runtime-поля (readonly) пишет только владелец домена.
    if (meta->readonly && meta->domain != owner) {
        ESP_LOGW(TAG, "write denied: field '%s' (domain=%d) by owner=%d",
                 meta->name, static_cast<int>(meta->domain), static_cast<int>(owner));
        return FieldWriteStatus::READONLY_DENIED;
    }

    if (!value || len == 0) return FieldWriteStatus::BAD_LENGTH;

    if (!valueInRange(meta, static_cast<const uint8_t*>(value), len,
                      meta->minVal, meta->maxVal)) {
        return FieldWriteStatus::OUT_OF_RANGE;
    }

    if (!deserializeField(meta, &data_, static_cast<const uint8_t*>(value), len)) {
        return FieldWriteStatus::BAD_LENGTH;
    }
    return FieldWriteStatus::OK;
}