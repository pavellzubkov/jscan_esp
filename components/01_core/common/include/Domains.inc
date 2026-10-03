#if defined(__INTELLISENSE__) || defined(__CLANGD_ANALYSIS__)
#include "AppTypes.hpp"
#ifndef DATA_FIELD
#define DATA_FIELD(type, name, def, min, max, validator, isConfig, readonly) type name;
#endif
#ifndef DOMAIN_ENTRY
#define DOMAIN_ENTRY(...)
#endif
#endif

// Единый список доменов системы. Включается с разными определениями
// DOMAIN_ENTRY / DATA_FIELD:
//   AppTypes.hpp — генерация enum FieldDomain (DOMAIN_ENTRY(x) -> x,);
//   AppData.hpp  — 4 прохода (struct / defaults / meta / UID), DOMAIN_ENTRY пуст.
// FIELD_DOMAIN подмешивается для meta-прохода g_fieldMeta[].
// Порядок блоков определяет порядок enum, членов AppData и g_fieldMeta[]
// (enum: WIFI..SYSTEM, PROTOCOL дописывается вручную в AppTypes.hpp).
DOMAIN_ENTRY(WIFI)
#undef FIELD_DOMAIN
#define FIELD_DOMAIN FieldDomain::WIFI
#include "WifiFields.inc"
#undef FIELD_DOMAIN

DOMAIN_ENTRY(TWAI)
#undef FIELD_DOMAIN
#define FIELD_DOMAIN FieldDomain::TWAI
#include "TwaiFields.inc"
#undef FIELD_DOMAIN

DOMAIN_ENTRY(SNAPSHOT)
#undef FIELD_DOMAIN
#define FIELD_DOMAIN FieldDomain::SNAPSHOT
#include "SnapshotFields.inc"
#undef FIELD_DOMAIN

DOMAIN_ENTRY(SYSTEM)
#undef FIELD_DOMAIN
#define FIELD_DOMAIN FieldDomain::SYSTEM
#include "SystemFields.inc"
#undef FIELD_DOMAIN