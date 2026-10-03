#if defined(__INTELLISENSE__) || defined(__CLANGD_ANALYSIS__)
#include "AppTypes.hpp"
#ifndef DATA_FIELD
#define DATA_FIELD(type, name, def, min, max, validator, isConfig, readonly) type name;
#endif
#ifndef DATA_FIELD_DOMAIN
#define DATA_FIELD_DOMAIN(Type, member, File) type member;
#endif
#endif

// Реестр полей системы, сгруппированный по доменам.
//
// Единый источник истины для:
//   - struct AppData (плоская сериализуемая схема, см. AppData.hpp);
//   - g_fieldMeta[]  (FieldMeta: uid/name/offset/size/validator/границы);
//   - генератора фронтенда (scripts/gen_shema.ts в frontend/).
//
// Каждый домен лежит в своём файле *.inc. Порядок доменов ниже определяет
// порядок полей в реестре (на UID не влияет — UID считается хэшем от имени поля).
//
// UID каждого поля = fnv1a32(name) & 0xFFFF (см. fieldUid() в AppTypes.hpp).
// Коллизии и совпадение с FULL_ID (0xFFFF) ловятся static_assert'ом в AppData.hpp.

DATA_FIELD_DOMAIN(Wifi, wifi, WifiFields.inc)
DATA_FIELD_DOMAIN(Twai, twai, TwaiFields.inc)
DATA_FIELD_DOMAIN(Snapshot, snapshot, SnapshotFields.inc)
DATA_FIELD_DOMAIN(System, system, SystemFields.inc)
