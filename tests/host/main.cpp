// Host-тесты без IDF (C++17). Запуск:
//   cmake -S tests/host -B build/host && cmake --build build/host && ./build/host/host_tests
// Покрытие: J1939Proto, FieldRegistry, SnapshotAccumulator, Timing::computeWaitMs.

#include "AppData.h"
#include "FieldRegistry.h"
#include "J1939Proto.h"
#include "SnapshotAccumulator.h"
#include "SystemTiming.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

// Строка в wire-формат поля: {len u8, bytes} (PROTOCOL §2).
static size_t strWire(char* out, const char* s) {
    size_t n = std::strlen(s);
    out[0] = static_cast<char>(n);
    std::memcpy(out + 1, s, n);
    return 1 + n;
}

// ============================================================
// J1939Proto
// ============================================================
static void test_proto() {
    // crc16: классический вектор CRC-16/CCITT-FALSE ("123456789" → 0x29B1).
    CHECK(J1939Proto::crc16(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0x29B1);
    CHECK(J1939Proto::crc16(nullptr, 0) == 0xFFFF);   // init value
    CHECK(J1939Proto::kMaxBatchPayload == 8192);

    // batchPayloadSize / serializeBatch / лимит 8192.
    uint8_t d1[8] = {1}, d2[8] = {2}, d3[8] = {3};
    J1939Proto::BatchRecord recs[3] = {
        {0x19, 0x123456, 8, d1, 250},
        {0x19, 0x123457, 8, d2, 250},
        {0x19, 0x123458, 8, d3, 250},
    };
    const size_t need = J1939Proto::batchPayloadSize(recs, 3);
    CHECK(need == 1 + 3 * (1 + 3 + 1 + 8 + 2));   // 1 + 3*15 = 46

    uint8_t buf[64];
    size_t n = J1939Proto::serializeBatch(buf, sizeof(buf), recs, 3);
    CHECK(n == need);
    CHECK(buf[0] == 3);   // count
    // Не влезает в маленький буфер.
    CHECK(J1939Proto::serializeBatch(buf, 10, recs, 3) == 0);

    // wrapFrame / unwrapFrame (round-trip + битые кадры).
    uint8_t payload[3] = {1, 0xAA, 0xBB};
    uint8_t frame[32];
    const size_t flen = J1939Proto::wrapFrame(
        J1939Proto::kMsgTypeSnapshot, J1939Proto::kFlagSnapshot,
        payload, sizeof(payload), 0x1234, frame, sizeof(frame));
    CHECK(flen == J1939Proto::kHeaderSize + 3 + J1939Proto::kCrcSize);
    CHECK(frame[0] == 0x5A && frame[1] == 0xA5);
    CHECK(frame[2] == 1);
    CHECK(frame[3] == J1939Proto::kFlagSnapshot);
    CHECK(frame[4] == 0x01 && frame[5] == 0x00);   // MsgType LE
    CHECK(frame[8] == 0x34 && frame[9] == 0x12);   // seq LE

    uint16_t mt = 0; uint8_t fl = 0;
    const uint8_t* pl = nullptr; size_t plen = 0;
    CHECK(J1939Proto::unwrapFrame(frame, flen, &mt, &fl, &pl, &plen));
    CHECK(mt == J1939Proto::kMsgTypeSnapshot && fl == J1939Proto::kFlagSnapshot);
    CHECK(plen == 3 && pl && std::memcmp(pl, payload, 3) == 0);

    // Слишком маленький кадр.
    CHECK(!J1939Proto::unwrapFrame(frame, 4, &mt, &fl, &pl, &plen));
    // Битый magic.
    uint8_t bad[32];
    std::memcpy(bad, frame, flen);
    bad[0] = 0xFF;
    CHECK(!J1939Proto::unwrapFrame(bad, flen, &mt, &fl, &pl, &plen));
    // Битый CRC.
    std::memcpy(bad, frame, flen);
    bad[flen - 1] ^= 0xFF;
    CHECK(!J1939Proto::unwrapFrame(bad, flen, &mt, &fl, &pl, &plen));
    // wrapFrame не влезает.
    CHECK(J1939Proto::wrapFrame(J1939Proto::kMsgTypeSnapshot, 0, payload, 3,
                                1, frame, 4) == 0);
}

// ============================================================
// FieldRegistry
// ============================================================
static void test_field_registry() {
    // fnv1a32/UID: константы схемы согласованы с реестром.
    CHECK(fieldUid("apSsid") == apSsid_UID);
    CHECK(fieldUid("canBitrate") == canBitrate_UID);
    CHECK(fieldUid("snapshotIntervalMs") == snapshotIntervalMs_UID);
    CHECK(apSsid_UID != apPassword_UID);
    CHECK(canBitrate_UID != apChannel_UID);
    CHECK(apSsid_UID != 0xFFFF && canBitrate_UID != 0xFFFF);

    AppData data;
    initAppDataDefault(data);
    std::recursive_mutex mutex;
    FieldRegistry reg(data, mutex);

    // Дефолты через getByName (raw).
    uint8_t ch = 0;
    CHECK(reg.getByName("apChannel", ch) && ch == 6);
    uint16_t timeout = 0;
    CHECK(reg.getByName("canTxTimeoutMs", timeout) && timeout == 100);

    // Запись uint32 (canBitrate) + чтение сериализованного значения.
    const uint32_t br = 500000;
    CHECK(reg.writeField(canBitrate_UID, &br, sizeof(br), FieldDomain::PROTOCOL) ==
          FieldWriteStatus::OK);
    uint32_t br2 = 0;
    size_t len = 0;
    CHECK(reg.readField(canBitrate_UID, &br2, sizeof(br2), &len));
    CHECK(len == sizeof(uint32_t) && br2 == 500000);

    // Диапазоны: apChannel вне [1,11].
    const uint8_t chBad = 20;
    CHECK(reg.writeField(apChannel_UID, &chBad, 1, FieldDomain::PROTOCOL) ==
          FieldWriteStatus::OUT_OF_RANGE);

    // Readonly: runtime-поле TWAI не пишется по протоколу, но пишется владельцем.
    const uint8_t st = 1;
    CHECK(reg.writeField(twaiState_UID, &st, 1, FieldDomain::PROTOCOL) ==
          FieldWriteStatus::READONLY_DENIED);
    CHECK(reg.writeFieldScalar(twaiState_UID, st) == FieldWriteStatus::OK);

    // Строка (CFG_STRING): запись/чтение wire-формата.
    char wire[80];
    const char* ssid = "JSCAN_AP";
    const size_t ws = strWire(wire, ssid);
    CHECK(reg.writeField(apSsid_UID, wire, ws, FieldDomain::PROTOCOL) ==
          FieldWriteStatus::OK);
    uint8_t out[40];
    size_t outLen = 0;
    CHECK(reg.readField(apSsid_UID, out, sizeof(out), &outLen));
    CHECK(outLen == 1 + std::strlen(ssid));
    CHECK(out[0] == std::strlen(ssid) && std::memcmp(out + 1, ssid, out[0]) == 0);

    // WPA2-пароль (CFG_PASSWORD): 0 или 8..63 символов.
    const char* p1 = "1234567";   // 7 — недопустимо
    const size_t w1 = strWire(wire, p1);
    CHECK(reg.writeField(apPassword_UID, wire, w1, FieldDomain::PROTOCOL) ==
          FieldWriteStatus::OUT_OF_RANGE);

    const char* p2 = "12345678";  // ровно 8 — допустимо
    const size_t w2 = strWire(wire, p2);
    CHECK(reg.writeField(apPassword_UID, wire, w2, FieldDomain::PROTOCOL) ==
          FieldWriteStatus::OK);

    const char* p3 = "";          // пустой = открытая сеть
    const size_t w3 = strWire(wire, p3);
    CHECK(reg.writeField(apPassword_UID, wire, w3, FieldDomain::PROTOCOL) ==
          FieldWriteStatus::OK);

    char longpw[65];
    std::memset(longpw, 'A', 64);
    longpw[64] = '\0';
    const size_t w4 = strWire(wire, longpw);   // 64 — недопустимо
    CHECK(reg.writeField(apPassword_UID, wire, w4, FieldDomain::PROTOCOL) ==
          FieldWriteStatus::OUT_OF_RANGE);
}

// ============================================================
// SnapshotAccumulator
// ============================================================
static void test_snapshot_accumulator() {
    const uint8_t d[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    const SnapshotAccumulator::Record* recs = nullptr;

    // Счёт и periodMs.
    SnapshotAccumulator acc;
    acc.update(1, 0x100, d, 8, 100);
    acc.update(2, 0x200, d, 8, 100);
    CHECK(acc.count() == 2);
    acc.update(1, 0x100, d, 8, 150);   // обновление той же записи
    CHECK(acc.count() == 2);
    size_t n = acc.collect(recs, 200, 1000);
    CHECK(n == 2);
    bool saw100 = false;
    for (size_t i = 0; i < n; ++i) {
        if (recs[i].sa == 1 && recs[i].pgn == 0x100) {
            saw100 = true;
            CHECK(recs[i].periodMs == 50);   // 150 - 100
        }
    }
    CHECK(saw100);

    // TTL: старые записи не попадают в collect.
    SnapshotAccumulator acc2;
    acc2.update(1, 0x100, d, 8, 100);
    acc2.update(2, 0x200, d, 8, 2000);
    n = acc2.collect(recs, 2100, 1000);   // активна только (2,0x200)
    CHECK(n == 1);
    CHECK(recs[0].sa == 2 && recs[0].pgn == 0x200);

    // Вытеснение при заполненной карте (128 записей).
    SnapshotAccumulator acc3;
    for (int i = 0; i < 128; ++i)
        acc3.update(1, static_cast<uint32_t>(i), d, 8, i + 1);
    CHECK(acc3.count() == 128);
    acc3.update(2, 0xFFFF, d, 8, 200);   // карта полна → вытесняется самая старая
    CHECK(acc3.count() == 128);
    n = acc3.collect(recs, 300, 100000);
    CHECK(n == 128);
    bool hasOldest = false, hasNew = false;
    for (size_t i = 0; i < n; ++i) {
        if (recs[i].sa == 1 && recs[i].pgn == 0) hasOldest = true;
        if (recs[i].sa == 2 && recs[i].pgn == 0xFFFF) hasNew = true;
    }
    CHECK(!hasOldest);   // самая старая (1,0) вытеснена
    CHECK(hasNew);

    // Длинное сообщение (TP) → heap-буфер; короткое → inline.
    SnapshotAccumulator acc4;
    uint8_t big[100];
    std::memset(big, 0xAB, sizeof(big));
    acc4.update(1, 0x500, big, sizeof(big), 100);
    n = acc4.collect(recs, 200, 1000);
    CHECK(n == 1);
    CHECK(recs[0].len == 100);
    CHECK(recs[0].data() != recs[0].smallData);   // bigData
    CHECK(std::memcmp(recs[0].data(), big, sizeof(big)) == 0);

    uint8_t small[4] = {1, 2, 3, 4};
    acc4.update(1, 0x500, small, sizeof(small), 150);
    n = acc4.collect(recs, 200, 1000);
    CHECK(n == 1 && recs[0].len == 4);
    CHECK(recs[0].data() == recs[0].smallData);   // bigData освобождён
    CHECK(std::memcmp(recs[0].data(), small, sizeof(small)) == 0);
}

// ============================================================
// Timing (чистая функция STEP-01)
// ============================================================
static void test_timing() {
    CHECK(Timing::computeWaitMs(1000, 2000, 500) == 500);
    CHECK(Timing::computeWaitMs(1000, 2000, 900) == 100);
    CHECK(Timing::computeWaitMs(1000, 1500, 500) == 500);   // телеметрия ближе
    CHECK(Timing::computeWaitMs(1000, 2000, 1000) == 1);    // снапшот наступил
    CHECK(Timing::computeWaitMs(1000, 2000, 1001) == 1);    // просрочен
    CHECK(Timing::computeWaitMs(1000, 2000, 3000) == 1);    // оба просрочены
    // Переполнение uint32: now обогнал next (мс каждые ~49 суток).
    CHECK(Timing::computeWaitMs(0xFFFFFFF0, 0xFFFFFFF0, 10) == 1);
}

int main() {
    test_proto();
    test_field_registry();
    test_snapshot_accumulator();
    test_timing();

    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}