// Host-тесты без IDF (C++17). Запуск:
//   cmake -S tests/host -B build/host && cmake --build build/host && ./build/host/host_tests
// Покрытие: J1939Proto, FieldRegistry, SnapshotAccumulator, Timing::computeWaitMs,
// J1939TransportProtocol (BAM/RTS/CTS/EOM), J1939Decoder.

#include "AppData.h"
#include "FieldRegistry.h"
#include "J1939Decoder.h"
#include "J1939Proto.h"
#include "J1939TransportProtocol.h"
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
    CHECK(need == 1 + 3 * (1 + 3 + 2 + 8 + 2));   // 1 + 3*16 = 49

    uint8_t buf[64];
    size_t n = J1939Proto::serializeBatch(buf, sizeof(buf), recs, 3);
    CHECK(n == need);
    CHECK(buf[0] == 3);   // count
    // Не влезает в маленький буфер.
    CHECK(J1939Proto::serializeBatch(buf, 10, recs, 3) == 0);

    // Запись с len=300 (>255): длина поля — uint16 LE, данные не усечься.
    uint8_t d300[300];
    std::memset(d300, 0x5A, sizeof(d300));
    J1939Proto::BatchRecord big{0x19, 0x123456, 300, d300, 1000};
    const size_t needBig = J1939Proto::batchPayloadSize(&big, 1);
    CHECK(needBig == 1 + (1 + 3 + 2 + 300 + 2));   // 309
    uint8_t bigBuf[400];
    size_t nb = J1939Proto::serializeBatch(bigBuf, sizeof(bigBuf), &big, 1);
    CHECK(nb != 0 && nb == needBig);
    CHECK(bigBuf[5] == 0x2C && bigBuf[6] == 0x01);           // len=300 LE
    CHECK(std::memcmp(bigBuf + 7, d300, 300) == 0);          // data целиком
    CHECK(bigBuf[307] == 0xE8 && bigBuf[308] == 0x03);       // periodMs=1000 LE
    // Буфер меньше нужного — 0.
    CHECK(J1939Proto::serializeBatch(bigBuf, needBig - 1, &big, 1) == 0);

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

    // Дефолты через getByUid (UID-константы): тот же raw-доступ, но опечатка
    // в имени не скомпилируется. FULL_ID (0xFFFF) — невалидный UID -> false.
    uint8_t chByUid = 0;
    CHECK(reg.getByUid(apChannel_UID, chByUid) && chByUid == 6);
    uint8_t chBadUid = 0;
    CHECK(!reg.getByUid(FULL_ID, chBadUid));

    // Дефолты полей обязаны совпадать с константами Hw/Timing (единый
    // источник литералов в HardwareConfig.h/SystemTiming.h).
    uint8_t nodeAddr = 0;
    CHECK(reg.getByName("canNodeAddr", nodeAddr) &&
          nodeAddr == Hw::kDefaultNodeAddr);
    uint32_t brDefault = 0;
    CHECK(reg.getByName("canBitrate", brDefault) &&
          brDefault == Hw::kCanBitrate);
    uint32_t snapIv = 0;
    CHECK(reg.getByName("snapshotIntervalMs", snapIv) &&
          snapIv == Timing::kSnapshotIntervalMs);
    uint32_t snapTtl = 0;
    CHECK(reg.getByName("snapshotTtlMs", snapTtl) &&
          snapTtl == Timing::kSnapshotTtlMs);

    // Запись uint32 (canBitrate) + чтение сериализованного значения.
    const uint32_t br = 500000;
    CHECK(reg.writeField(canBitrate_UID, &br, sizeof(br), FieldDomain::PROTOCOL) ==
          FieldWriteStatus::OK);
    uint32_t br2 = 0;
    size_t len = 0;
    CHECK(reg.readField(canBitrate_UID, &br2, sizeof(br2), &len));
    CHECK(len == sizeof(uint32_t) && br2 == 500000);

    // Строгие длины: wire-формат числа = ровно meta->size байт.
    // Payload длиннее size раньше принимался (хвост молча отрезался).
    uint8_t tooLong[sizeof(uint32_t) + 1] = {};
    CHECK(reg.writeField(canBitrate_UID, tooLong, sizeof(tooLong),
                         FieldDomain::PROTOCOL) ==
          FieldWriteStatus::OUT_OF_RANGE);
    uint8_t tooShort[sizeof(uint32_t) - 1] = {};
    CHECK(reg.writeField(canBitrate_UID, tooShort, sizeof(tooShort),
                         FieldDomain::PROTOCOL) ==
          FieldWriteStatus::OUT_OF_RANGE);

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

    // writeFieldString: C-строка → wire {len, bytes} собирается сам.
    CHECK(reg.writeFieldString(apSsid_UID, "NEW_SSID") == FieldWriteStatus::OK);
    CHECK(reg.readField(apSsid_UID, out, sizeof(out), &outLen));
    CHECK(outLen == 1 + 8);
    CHECK(out[0] == 8 && std::memcmp(out + 1, "NEW_SSID", 8) == 0);

    // 7 символов — недопустимый WPA2-пароль.
    CHECK(reg.writeFieldString(apPassword_UID, "1234567") ==
          FieldWriteStatus::OUT_OF_RANGE);

    // Строка в числовое поле — защита от неверного вызова.
    CHECK(reg.writeFieldString(canBitrate_UID, "500000") ==
          FieldWriteStatus::BAD_LENGTH);

    // CFG_IP: сверх длины (7..15) проверяются октеты (0..255, ровно 4).
    const size_t wip = strWire(wire, "10.10.10.10");
    CHECK(reg.writeField(apIp_UID, wire, wip, FieldDomain::PROTOCOL) ==
          FieldWriteStatus::OK);
    const size_t wip1 = strWire(wire, "999.1.1.1");   // октет > 255
    CHECK(reg.writeField(apIp_UID, wire, wip1, FieldDomain::PROTOCOL) ==
          FieldWriteStatus::OUT_OF_RANGE);
    const size_t wip2 = strWire(wire, "1.2.3");        // всего 3 октета
    CHECK(reg.writeField(apIp_UID, wire, wip2, FieldDomain::PROTOCOL) ==
          FieldWriteStatus::OUT_OF_RANGE);
    const size_t wip3 = strWire(wire, "10.10.10.");    // хвостовая точка
    CHECK(reg.writeField(apIp_UID, wire, wip3, FieldDomain::PROTOCOL) ==
          FieldWriteStatus::OUT_OF_RANGE);
    const size_t wip4 = strWire(wire, "a.b.c.d");      // не цифры
    CHECK(reg.writeField(apIp_UID, wire, wip4, FieldDomain::PROTOCOL) ==
          FieldWriteStatus::OUT_OF_RANGE);
    const size_t wip5 = strWire(wire, "10.10..10");    // пустой октет
    CHECK(reg.writeField(apIp_UID, wire, wip5, FieldDomain::PROTOCOL) ==
          FieldWriteStatus::OUT_OF_RANGE);

    // Значение после отказа не изменилось (последняя валидная запись).
    CHECK(reg.readField(apIp_UID, out, sizeof(out), &outLen));
    CHECK(outLen == 1 + 11 && std::memcmp(out + 1, "10.10.10.10", 11) == 0);
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

    // Регрессия UAF: большая запись, collect() дважды подряд (уплотнение
    // обязано переносить владение bigData, а не дублировать), затем
    // update() того же ключа с bigData и ещё collect().
    SnapshotAccumulator acc5;
    uint8_t big2[64];
    for (size_t i = 0; i < sizeof(big2); ++i)
        big2[i] = static_cast<uint8_t>(i);
    acc5.update(1, 0x600, big2, sizeof(big2), 100);
    acc5.update(2, 0x601, d, 8, 110);        // сдвиг при уплотнении
    n = acc5.collect(recs, 150, 1000);
    CHECK(n == 2);
    n = acc5.collect(recs, 160, 1000);       // второй collect без изменений
    CHECK(n == 2);
    CHECK(recs[0].data() != recs[0].smallData &&
          std::memcmp(recs[0].data(), big2, sizeof(big2)) == 0);
    uint8_t big3[100];
    std::memset(big3, 0xCD, sizeof(big3));
    acc5.update(1, 0x600, big3, sizeof(big3), 200);   // перезапись bigData
    n = acc5.collect(recs, 250, 1000);
    CHECK(n == 2);
    bool found600 = false;
    for (size_t i = 0; i < n; ++i) {
        if (recs[i].sa == 1 && recs[i].pgn == 0x600) {
            found600 = true;
            CHECK(recs[i].len == 100);
            CHECK(std::memcmp(recs[i].data(), big3, sizeof(big3)) == 0);
        }
    }
    CHECK(found600);
    // Вытеснение после дублирования не должно ронять программу (ASan).
    acc5.update(3, 0x602, big3, sizeof(big3), 300);
    acc5.update(4, 0x603, big3, sizeof(big3), 301);
    n = acc5.collect(recs, 350, 100000);
    CHECK(n == 4);
    CHECK(acc5.count() == 4);

    // 128 слотов большими записей → collect() → вытеснение старой.
    SnapshotAccumulator acc6;
    uint8_t bigRec[40];
    std::memset(bigRec, 0xEE, sizeof(bigRec));
    for (int i = 0; i < 128; ++i)
        acc6.update(1, static_cast<uint32_t>(i), bigRec, sizeof(bigRec),
                    static_cast<uint32_t>(i) + 1);
    CHECK(acc6.count() == 128);
    n = acc6.collect(recs, 200, 100000);
    CHECK(n == 128);
    CHECK(acc6.count() == 128);
    acc6.update(2, 0xFFFF, bigRec, sizeof(bigRec), 300);   // вытеснение
    CHECK(acc6.count() == 128);
    n = acc6.collect(recs, 400, 100000);
    CHECK(n == 128);
    CHECK(acc6.count() == 128);

    // TTL-инвалидация: протухшая запись с bigData освобождает слот.
    SnapshotAccumulator acc7;
    acc7.update(1, 0x700, big2, sizeof(big2), 100);
    CHECK(acc7.count() == 1);
    n = acc7.collect(recs, 100 + 1001, 1000);   // TTL истёк
    CHECK(n == 0);
    CHECK(acc7.count() == 0);
    // Слот свободен для новой записи.
    acc7.update(2, 0x701, d, 8, 5000);
    CHECK(acc7.count() == 1);
    n = acc7.collect(recs, 5000, 1000);
    CHECK(n == 1 && recs[0].sa == 2);
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

// ============================================================
// J1939Decoder (29-бит CAN ID → PGN/SA/dst)
// ============================================================
static void test_decoder() {
    // PDU1 (PF=0xEC < 240): TP.CM, PS = destination address.
    TwaiDriver::RxFrame f{};
    f.id = (6u << 26) | (0xECu << 16) | (0x20u << 8) | 0x19u;   // dst=0x20 sa=0x19
    f.dlc = 8;
    f.data[0] = 32;
    J1939PgnMsg m = J1939Decoder::decode(f);
    CHECK(m.priority == 6);
    CHECK(m.pgn == 0xEC00);      // PS вырезан из PGN
    CHECK(m.dst == 0x20);        // но сохранён как адресат
    CHECK(m.sa == 0x19);
    CHECK(m.isP2P);

    // PDU2 (PF=0xFF >= 240): PGN групповой, dst = broadcast.
    f.id = (6u << 26) | (0xFF12u << 8) | 0x19u;
    m = J1939Decoder::decode(f);
    CHECK(m.pgn == 0xFF12);
    CHECK(m.dst == 0xFF);
    CHECK(!m.isP2P);

    // dlc > 8 усекается до 8.
    f.dlc = 15;
    m = J1939Decoder::decode(f);
    CHECK(m.dlc == 8);

    // peerToPeer: границы PDU1-диапазонов.
    CHECK(!J1939Decoder::peerToPeer(0));
    CHECK(J1939Decoder::peerToPeer(0xEFFF));
    CHECK(!J1939Decoder::peerToPeer(0xF000));
    CHECK(!J1939Decoder::peerToPeer(0x10000));
    CHECK(J1939Decoder::peerToPeer(0x10001));
    CHECK(J1939Decoder::peerToPeer(0x1EFFF));
    CHECK(!J1939Decoder::peerToPeer(0x1F000));
}

// ============================================================
// J1939TransportProtocol (BAM + RTS/CTS/EOM + таймауты)
// ============================================================
// Тестовые конструкторы TP.CM/TP.DT.
static J1939PgnMsg makeCm(uint8_t control, uint8_t sa, uint8_t dst,
                          uint16_t len, uint8_t packets, uint32_t pgn,
                          uint8_t dlc = 8) {
    J1939PgnMsg cm{};
    cm.pgn = 60416;   // TP.CM
    cm.sa = sa;
    cm.dst = dst;
    cm.dlc = dlc;
    cm.data[0] = control;
    cm.data[1] = static_cast<uint8_t>(len & 0xFF);
    cm.data[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
    cm.data[3] = packets;
    cm.data[4] = 0xFF;
    cm.data[5] = static_cast<uint8_t>(pgn & 0xFF);
    cm.data[6] = static_cast<uint8_t>((pgn >> 8) & 0xFF);
    cm.data[7] = static_cast<uint8_t>((pgn >> 16) & 0xFF);
    return cm;
}

// TP.DT: data[0] = номер пакета (1..255), data[1..7] = 7 байт данных.
static J1939PgnMsg makeDt(uint8_t sa, uint8_t packetN, uint8_t fillBase,
                          uint8_t dlc = 8) {
    J1939PgnMsg dt{};
    dt.pgn = 60160;   // TP.DT
    dt.sa = sa;
    dt.dst = 0xFF;
    dt.dlc = dlc;
    dt.data[0] = packetN;
    for (int b = 1; b <= 7; ++b)
        dt.data[b] = static_cast<uint8_t>(fillBase + b - 1);
    return dt;
}

static void test_transport_protocol() {
    const uint8_t kLocalAddr = 25;
    J1939TransportProtocol tp;
    J1939AssembledMsg out{};
    TpAction act;

    // ---------- (1) BAM happy-path: 1785 байт = 255 пакетов ----------
    tp.setLocalAddr(kLocalAddr);
    tp.reset();
    // PGN 0xFE00 (65024), len=1785=0x06F9, packets=255.
    J1939PgnMsg bam = makeCm(32, 0x10, 0xFF, 1785, 255, 0xFE00);
    act = tp.onTpCm(bam, 0);
    CHECK(act.kind == TpAction::Kind::None);   // BAM — ответов не требует

    bool complete = false;
    for (int i = 1; i <= 255 && !complete; ++i) {
        J1939PgnMsg dt = makeDt(0x10, static_cast<uint8_t>(i),
                                static_cast<uint8_t>((i - 1) * 7));
        act = TpAction{};
        complete = tp.onTpDt(dt, static_cast<uint32_t>(i * 10), out, &act);
        if (!complete)
            CHECK(act.kind == TpAction::Kind::None);
    }
    CHECK(complete);
    CHECK(out.len == 1785);
    CHECK(out.pgn == 0xFE00);
    CHECK(out.sa == 0x10);
    CHECK(act.kind == TpAction::Kind::None);   // BAM — без EOM
    CHECK(out.data[0] == 0 && out.data[6] == 6);    // пакет 1: fill 0..6
    CHECK(out.data[7] == 7 && out.data[13] == 13);  // пакет 2: fill 7..13
    // Пакет 255: off = 254*7 = 1778, fill = uint8_t(1778) — хвост влез целиком.
    CHECK(out.data[1778] == static_cast<uint8_t>(254 * 7));
    CHECK(out.data[1784] == static_cast<uint8_t>(254 * 7 + 6));

    // ---------- (2) BAM с неверным packets -> сессия не создаётся ----------
    tp.reset();
    J1939PgnMsg badBam = makeCm(32, 0x10, 0xFF, 1785, 100, 0xFE00);  // != 255
    act = tp.onTpCm(badBam, 0);
    CHECK(act.kind == TpAction::Kind::None);
    J1939PgnMsg dt1 = makeDt(0x10, 1, 0);
    CHECK(!tp.onTpDt(dt1, 10, out, &act));   // сессии нет — пакет отбит

    // ---------- (3) RTS с dlc < 8 -> ignore ----------
    tp.reset();
    J1939PgnMsg shortRts = makeCm(16, 0x30, kLocalAddr, 14, 2, 0x123456);
    shortRts.dlc = 7;                        // обрезан — data[7] мусор
    act = tp.onTpCm(shortRts, 0);
    CHECK(act.kind == TpAction::Kind::None);
    J1939PgnMsg dt2 = makeDt(0x30, 1, 0);
    CHECK(!tp.onTpDt(dt2, 10, out, &act));   // сессия не создана

    // ---------- (4) RTS на наш адрес -> CTS + активный приём + EOM ----------
    tp.reset();
    J1939PgnMsg rts = makeCm(16, 0x30, kLocalAddr, 14, 2, 0x123456);
    act = tp.onTpCm(rts, 100);
    CHECK(act.kind == TpAction::Kind::SendCts);
    CHECK(act.dst == 0x30);                  // ответ — отправителю RTS
    CHECK(act.packets == 2);                 // ceil(14/7)
    CHECK(act.totalLen == 14);
    CHECK(act.totalPackets == 2);
    CHECK(act.pgn == 0x123456);

    J1939PgnMsg adt1 = makeDt(0x30, 1, 0x40);
    act = TpAction{};
    CHECK(!tp.onTpDt(adt1, 110, out, &act));
    CHECK(act.kind == TpAction::Kind::None);           // ещё не конец
    J1939PgnMsg adt2 = makeDt(0x30, 2, 0x50);
    act = TpAction{};
    CHECK(tp.onTpDt(adt2, 120, out, &act));            // последний пакет
    CHECK(out.len == 14 && out.sa == 0x30 && out.pgn == 0x123456);
    CHECK(out.data[0] == 0x40 && out.data[6] == 0x46); // пакет 1
    CHECK(out.data[7] == 0x50 && out.data[13] == 0x56);// пакет 2 (chunk 7)
    CHECK(act.kind == TpAction::Kind::SendEom);        // активный приём -> EOM
    CHECK(act.dst == 0x30);                            // EOM -> отправителю
    CHECK(act.totalLen == 14 && act.totalPackets == 2);
    CHECK(act.pgn == 0x123456);

    // ---------- (5) RTS на чужой адрес -> пассивный сниффинг, без ответов ----
    tp.reset();
    J1939PgnMsg prts = makeCm(16, 0x30, 0x40, 14, 2, 0xABCDEF);
    act = tp.onTpCm(prts, 200);
    CHECK(act.kind == TpAction::Kind::None);           // не отвечаем!
    J1939PgnMsg pdt1 = makeDt(0x30, 1, 0x10);
    act = TpAction{};
    CHECK(!tp.onTpDt(pdt1, 210, out, &act));
    J1939PgnMsg pdt2 = makeDt(0x30, 2, 0x20);
    act = TpAction{};
    CHECK(tp.onTpDt(pdt2, 220, out, &act));            // собрано
    CHECK(out.len == 14 && out.pgn == 0xABCDEF);
    CHECK(out.data[0] == 0x10 && out.data[7] == 0x20);
    CHECK(act.kind == TpAction::Kind::None);           // пассивный — без EOM!

    // ---------- (5b) EOM от чужого получателя закрывает пассивную сессию ----
    tp.reset();
    act = tp.onTpCm(makeCm(16, 0x30, 0x40, 14, 2, 0xABCDEF), 300);
    CHECK(act.kind == TpAction::Kind::None);
    CHECK(!tp.onTpDt(makeDt(0x30, 1, 0x10), 310, out, &act));
    // EOM: от получателя (0x40) отправителю (0x30), control=19.
    J1939PgnMsg eom = makeCm(19, 0x40, 0x30, 14, 2, 0xABCDEF);
    act = tp.onTpCm(eom, 320);
    CHECK(act.kind == TpAction::Kind::None);           // закрытие — без ответа
    CHECK(!tp.onTpDt(makeDt(0x30, 2, 0x20), 330, out, &act)); // сессии нет

    // ---------- (6) таймаут: tick() закрывает брошенную сессию ----------
    tp.reset();
    act = tp.onTpCm(makeCm(16, 0x30, 0x40, 14, 2, 0xABCDEF), 1000);
    CHECK(!tp.onTpDt(makeDt(0x30, 1, 0x10), 1050, out, &act));
    tp.tick(1050 + Timing::kTransportTimeoutMs);        // sweep по краю таймаута
    CHECK(!tp.onTpDt(makeDt(0x30, 2, 0x20), 2100, out, &act)); // сессия закрыта

    // DT с dlc < 2 игнорируется.
    tp.reset();
    tp.onTpCm(makeCm(32, 0x10, 0xFF, 14, 2, 0xFE00), 0);
    J1939PgnMsg tiny = makeDt(0x10, 1, 0, /*dlc=*/1);
    CHECK(!tp.onTpDt(tiny, 10, out, &act));
}

int main() {
    test_proto();
    test_field_registry();
    test_snapshot_accumulator();
    test_timing();
    test_decoder();
    test_transport_protocol();

    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}