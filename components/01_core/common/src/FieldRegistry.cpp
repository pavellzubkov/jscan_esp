#include "FieldRegistry.h"
#include "AppTypes.h"
#include "ByteOrder.h"   // readUnsignedLE/readSignedLE — общий с ConfigStore
#include "esp_log.h"
#include <cstring>
#include <mutex>

static const char* TAG = "FieldRegistry";

// ============================================================
// Сериализация / десериализация значений полей (по PROTOCOL §2)
// (вынесено из CommunicationModule — единый источник для comm/ConfigStore)
// ============================================================

// Размер сериализованного представления одного поля (без padding; строки — с префиксом длины).
static size_t serializedFieldSize(const FieldMeta* meta, const AppData* data) {
    if (meta->validator == CFG_STRING || meta->validator == CFG_IP ||
        meta->validator == CFG_PASSWORD) {
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
    if (meta->validator == CFG_STRING || meta->validator == CFG_IP ||
        meta->validator == CFG_PASSWORD) {
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

// Строка — корректный IPv4 "a.b.c.d": ровно 4 октета, каждый 0..255,
// только цифры и точки, без пустых октетов и хвоста.
// (Длина по min/max из meta не проверяется — это ответственность вызывающего.)
static bool isValidIpv4(const char* s, size_t len) {
    if (!s || len == 0 || len > 15) return false;
    size_t i = 0;
    unsigned octets = 0;
    while (i < len) {
        if (s[i] < '0' || s[i] > '9') return false;   // пустой октет / не цифра
        unsigned v = 0;
        int digits = 0;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + static_cast<unsigned>(s[i] - '0');
            ++digits;
            ++i;
            if (digits > 3 || v > 255) return false;
        }
        if (++octets > 4) return false;
        if (i < len) {
            if (s[i] != '.') return false;
            ++i;
            if (i == len) return false;   // точка в конце
        }
    }
    return octets == 4;
}

static bool valueInRange(const FieldMeta* meta, const uint8_t* payload, size_t payloadLen,
                         double minVal, double maxVal) {
    if (meta->validator == CFG_STRING || meta->validator == CFG_IP ||
        meta->validator == CFG_PASSWORD) {
        if (payloadLen < 1) return false;
        uint8_t sl = payload[0];
        if (sl != payloadLen - 1) return false;          // длина не совпадает
        if (meta->validator == CFG_PASSWORD)
            return (sl == 0) || ((sl >= 8) && (sl <= maxVal));  // пустой или 8..63
        if ((sl < minVal) || (sl > maxVal)) return false;
        // CFG_IP: сверх длины — строгий разбор октетов (999.1.1.1 и пр.).
        if (meta->validator == CFG_IP)
            return isValidIpv4(reinterpret_cast<const char*>(payload + 1), sl);
        return true;
    }
    if (meta->validator == CFG_FLOAT) {
        if (payloadLen != sizeof(float)) return false;
        float f; memcpy(&f, payload, sizeof(float));
        double v = f;
        return (v >= minVal) && (v <= maxVal);
    }
    // Целые читаем как знаковые (допускают отрицательные значения);
    // enum / bool — как беззнаковые (0..N). Длина wire — строго meta->size:
    // более длинный payload молча усекался бы в deserializeField (хвост
    // терялся без ошибки), короткий и раньше отклонялся.
    if (payloadLen != meta->size) return false;
    double v = (meta->validator == CFG_INT)
                   ? static_cast<double>(readSignedLE(payload, meta->size))
                   : static_cast<double>(readUnsignedLE(payload, meta->size));
    return (v >= minVal) && (v <= maxVal);
}

static bool deserializeField(const FieldMeta* meta, AppData* data,
                             const uint8_t* payload, size_t payloadLen) {
    uint8_t* dst = reinterpret_cast<uint8_t*>(data) + meta->offset;
    if (meta->validator == CFG_STRING || meta->validator == CFG_IP ||
        meta->validator == CFG_PASSWORD) {
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
    if (payloadLen != meta->size) return false;
    memcpy(dst, payload, meta->size);
    return true;
}

// ============================================================
// FieldRegistry
// ============================================================

FieldRegistry::FieldRegistry(AppData& data, std::recursive_mutex& mutex)
    : data_(data), mutex_(mutex) {}

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

const FieldMeta* FieldRegistry::fieldAt(size_t index) const {
    if (index >= kAppFieldCount) return nullptr;
    return &g_fieldMeta[index];
}

void FieldRegistry::setDynamicReader(FieldDynamicReader reader) {
    dynamicReader_ = reader;
}

bool FieldRegistry::readField(uint16_t uid, void* out, size_t out_cap,
                              size_t* out_len) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const FieldMeta* meta = getMetaByUid(uid);
    if (!meta) return false;

    // Динамические поля считаются на лету провайдером (например, uptimeMs/
    // heapFree) — в AppData они не хранятся, поэтому обычный путь ниже
    // не используется.
    if (dynamicReader_ &&
        dynamicReader_(uid, static_cast<uint8_t*>(out), out_cap, out_len)) {
        return true;
    }

    size_t len = serializedFieldSize(meta, &data_);
    if (out_len) *out_len = len;
    if (!out) return true;   // запрос размера
    if (len > out_cap) return false;

    serializeField(meta, &data_, static_cast<uint8_t*>(out));
    return true;
}

bool FieldRegistry::readFieldRaw(uint16_t uid, void* out, size_t out_cap) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const FieldMeta* meta = getMetaByUid(uid);
    if (!meta) return false;
    if (meta->size > out_cap) return false;
    memcpy(out, reinterpret_cast<const uint8_t*>(&data_) + meta->offset,
           meta->size);
    return true;
}

FieldWriteStatus FieldRegistry::writeField(uint16_t uid, const void* value,
                                            size_t len, FieldDomain owner) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
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

FieldWriteStatus FieldRegistry::writeFieldDetectChange(uint16_t uid, const void* value,
                                                       size_t len, FieldDomain owner,
                                                       bool* changed) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Старое значение — до записи (нужно только для детекции).
    uint8_t oldBuf[kAppMaxFieldSize + 8];
    size_t oldLen = 0;
    const bool hadOld = changed &&
                        readField(uid, oldBuf, sizeof(oldBuf), &oldLen);

    const FieldWriteStatus st = writeField(uid, value, len, owner);
    if (st != FieldWriteStatus::OK) {
        if (changed) *changed = false;
        return st;
    }

    if (changed) {
        uint8_t newBuf[kAppMaxFieldSize + 8];
        size_t newLen = 0;
        const bool hasNew = readField(uid, newBuf, sizeof(newBuf), &newLen);
        *changed = !hadOld || !hasNew || oldLen != newLen ||
                   memcmp(oldBuf, newBuf, oldLen) != 0;
    }
    return FieldWriteStatus::OK;
}

FieldWriteStatus FieldRegistry::writeFieldString(uint16_t uid, const char* value) {
    const FieldMeta* meta = getMetaByUid(uid);
    if (!meta) return FieldWriteStatus::UNKNOWN_UID;
    if (meta->validator != CFG_STRING && meta->validator != CFG_IP &&
        meta->validator != CFG_PASSWORD) {
        return FieldWriteStatus::BAD_LENGTH;   // не строковое поле
    }
    if (!value) return FieldWriteStatus::BAD_LENGTH;
    size_t len = strlen(value);
    if (len > 255) return FieldWriteStatus::BAD_LENGTH;

    uint8_t buf[1 + 255];
    buf[0] = static_cast<uint8_t>(len);
    memcpy(buf + 1, value, len);
    return writeField(uid, buf, 1 + len, meta->domain);
}