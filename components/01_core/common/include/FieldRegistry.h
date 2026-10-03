#pragma once
#include "AppData.h"
#include "AppTypes.h"
#include <cstddef>
#include <cstdint>
#include <mutex>

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
// Все операции чтения/записи AppData выполняются под рекурсивным мьютексом
// AppContext.adataMutex (вложенные AppDataLock остаются корректными).
class FieldRegistry {
 public:
    explicit FieldRegistry(AppData& data, std::recursive_mutex& mutex);

    // --- Метаданные схемы ---
    const FieldMeta* getMetaByUid(uint16_t uid) const;
    const FieldMeta* getMetaByName(const char* name) const;
    size_t fieldCount() const;
    // Метаданные по индексу; nullptr при выходе за границы (вызывающий
    // обязан проверять указатель — раньше ссылка уходила за массив).
    const FieldMeta* fieldAt(size_t index) const;

    // Регистрация провайдера динамических полей (один на систему).
    // Вызывается один раз при старте, до запуска задач.
    void setDynamicReader(FieldDynamicReader reader);

    // --- Чтение ---
    // Копирует сериализованное значение поля (по правилам PROTOCOL.md) в out.
    // out может быть nullptr — тогда только out_len (для определения размера).
    // Возвращает false, если поля нет или буфер мал.
    bool readField(uint16_t uid, void* out, size_t out_cap,
                   size_t* out_len) const;

    // Копирует «сырое» значение поля (без сериализации) в out. Для строк —
    // сам FixedString целиком. Выполняется под локом. Возвращает false, если
    // поля нет или out_cap мал.
    bool readFieldRaw(uint16_t uid, void* out, size_t out_cap) const;

    // --- Запись (с валидацией и проверкой владельца) ---
    // value — сериализованное значение (см. PROTOCOL.md §2), len — его длина.
    // owner — домен пишущего (FieldDomain::WIFI/...); для протокола,
    // который пишет только config-поля, используйте FieldDomain::PROTOCOL —
    // главное, чтобы он не совпадал с доменом readonly-поля.
    FieldWriteStatus writeField(uint16_t uid, const void* value,
                                size_t len, FieldDomain owner);

    // Запись строкового поля (CFG_STRING/CFG_IP/CFG_PASSWORD) из C-строки:
    // сама конструирует wire {len, bytes} и вызывает writeField с доменом
    // поля. Для FixedString вместо writeFieldScalar — тот передал бы сырые
    // 64 байта, а валидатор ждёт префикс длины.
    FieldWriteStatus writeFieldString(uint16_t uid, const char* value);

    // Type-safe чтение «сырого» значения по имени (удобство; в рантайме
    // предпочтительнее getByUid — опечатка в имени тихо вернёт false).
    template <typename T>
    bool getByName(const char* name, T& out) const {
        const FieldMeta* m = getMetaByName(name);
        if (!m || m->size != sizeof(T)) return false;
        return readFieldRaw(m->uid, &out, sizeof(T));
    }

    // Type-safe чтение «сырого» значения по UID-константе (*_UID из AppData.h):
    // опечатка в имени UID — ошибка компиляции, а не тихий возврат false.
    // Для FixedString сверяется m->size == sizeof(T) — как в getByName.
    template <typename T>
    bool getByUid(uint16_t uid, T& out) const {
        const FieldMeta* m = getMetaByUid(uid);
        if (!m || m->size != sizeof(T)) return false;
        return readFieldRaw(m->uid, &out, sizeof(T));
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
    std::recursive_mutex& mutex_;   // AppContext.adataMutex (рекурсивный)
    FieldDynamicReader dynamicReader_ = nullptr;
};