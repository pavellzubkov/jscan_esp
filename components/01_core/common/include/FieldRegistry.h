#pragma once
#include "AppData.h"
#include "AppTypes.h"
#include <cstddef>
#include <cstdint>

// Опциональный провайдер «динамических» runtime-полей: вычисляет значение
// на лету в момент чтения (uptimeMs/heapFree и т.п.), вместо хранения в
// AppData. Вызывается под тем же мьютексом, что и обычное чтение, но не
// трогает AppData — сразу сериализует результат в out.
// Возвращает true, если поле обработано.
using FieldDynamicReader = bool (*)(uint16_t uid, uint8_t* out,
                                    size_t out_cap, size_t* out_len);

// Результат попытки записи поля.
enum class FieldWriteStatus : uint8_t {
    OK = 0,
    UNKNOWN_UID,      // поля с таким UID нет
    READONLY_DENIED,  // поле readonly, а пишущий не владелец его домена
    OUT_OF_RANGE,     // значение вне [minVal, maxVal]
    BAD_LENGTH        // неверная длина payload
};

// Реестр полей системы с контролем владения.
//
// Единственный интерфейс для чтения/записи полей AppData модулями и
// протоколом. Запись runtime-полей (readonly=true) разрешена только
// владельцу домена (см. FieldMeta.domain) — это превращает «карту владельцев»
// из документации в рантайм-гарантию. Config-поля (readonly=false) может
// писать любой валидный пишущий (протокол, автосейв, UI).
//
// Реализация не хранит данные — она оборачивает AppData и g_fieldMeta.
class FieldRegistry {
 public:
    explicit FieldRegistry(AppData& data);

    // --- Метаданные схемы ---
    const FieldMeta* getMetaByUid(uint16_t uid) const;
    const FieldMeta* getMetaByName(const char* name) const;
    size_t fieldCount() const;
    const FieldMeta& fieldAt(size_t index) const;

    // Регистрация провайдера динамических полей (один на систему).
    // Вызывается один раз при старте, до запуска задач.
    void setDynamicReader(FieldDynamicReader reader);

    // --- Чтение ---
    // Копирует сериализованное значение поля (по правилам PROTOCOL.md) в out.
    // out может быть nullptr — тогда только out_len (для определения размера).
    // Возвращает false, если поля нет или буфер мал.
    bool readField(uint16_t uid, void* out, size_t out_cap,
                   size_t* out_len) const;

    // --- Запись (с валидацией и проверкой владельца) ---
    // value — сериализованное значение (см. PROTOCOL.md §2), len — его длина.
    // owner — домен пишущего (FieldDomain::WIFI/...); для протокола,
    // который пишет только config-поля, используйте FieldDomain::PROTOCOL —
    // главное, чтобы он не совпадал с доменом readonly-поля.
    FieldWriteStatus writeField(uint16_t uid, const void* value,
                                size_t len, FieldDomain owner);

    // Type-safe чтение по имени (удобство).
    template <typename T>
    bool getByName(const char* name, T& out) const {
        const FieldMeta* m = getMetaByName(name);
        if (!m || m->size != sizeof(T)) return false;
        return readField(m->uid, &out, sizeof(T), nullptr);
    }

    // Запись типизированного (native, little-endian) скалярного значения.
    // Владельцем берётся домен самого поля (meta->domain) — для доверенного
    // писателя (агрегатор домена), который пишет runtime-поля своего домена.
    template <typename T>
    FieldWriteStatus writeFieldScalar(uint16_t uid, const T& value) {
        const FieldMeta* m = getMetaByUid(uid);
        if (!m || m->size != sizeof(T)) return FieldWriteStatus::BAD_LENGTH;
        return writeField(uid, &value, sizeof(T), m->domain);
    }

 private:
    AppData& data_;
    FieldDynamicReader dynamicReader_ = nullptr;
};