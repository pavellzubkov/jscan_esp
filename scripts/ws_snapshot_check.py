"""Читалка WS-снапшотов J1939-сканера.

Подключение к ws://10.10.10.10/ws (клиент должен быть подключён к AP J1939_AP),
печать заголовков бинарных кадров и проверка magic/CRC16 по docs/PROTOCOL-J1939.md.

Зависимость: pip install websockets
Запуск:     python scripts/ws_snapshot_check.py
"""

import asyncio
import sys

import websockets

MAGIC = (0x5A, 0xA5)
MSG_TYPE_SNAPSHOT = 0x0001
MSG_TYPE_REQUEST = 0x0002


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE (рефлексированная форма, как в J1939Proto::crc16)."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if (crc & 1) else (crc >> 1)
    return crc


def check_frame(msg: bytes) -> str:
    if len(msg) < 12:
        return f"короткий кадр {len(msg)}B"
    if msg[0] != MAGIC[0] or msg[1] != MAGIC[1]:
        return f"magic={msg[0]:02X}{msg[1]:02X} (ожидалось 5AA5)"
    if msg[2] != 1:
        return f"version={msg[2]} (ожидалось 1)"
    msg_type = int.from_bytes(msg[4:6], "little")
    payload_len = int.from_bytes(msg[6:8], "little")
    seq = int.from_bytes(msg[8:10], "little")
    if 10 + payload_len + 2 > len(msg):
        return "длина payload не сходится с размером кадра"
    crc_stored = int.from_bytes(msg[-2:], "little")
    crc_calc = crc16(msg[:-2])
    crc_ok = "CRC ok" if crc_stored == crc_calc else f"CRC FAIL {crc_stored:04X}!={crc_calc:04X}"
    type_name = "SNAPSHOT" if msg_type == MSG_TYPE_SNAPSHOT else (
        "REQUEST" if msg_type == MSG_TYPE_REQUEST else f"0x{msg_type:04X}"
    )
    records = msg[10] if len(msg) > 10 and msg_type == MSG_TYPE_SNAPSHOT else "?"
    return (f"{len(msg)}B magic=5AA5 ver={msg[2]} flags={msg[3]:02X} "
            f"type={type_name} payload={payload_len} seq={seq} "
            f"records={records} {crc_ok}")


async def main(uri: str, count: int = 20) -> None:
    async with websockets.connect(uri, max_size=2**20) as ws:
        print(f"connected {uri}")
        for _ in range(count):
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=5)
            except asyncio.TimeoutError:
                print("таймаут ожидания кадра (5 с)")
                continue
            if not isinstance(msg, bytes):
                continue
            print(f"frame {check_frame(msg)}")


if __name__ == "__main__":
    try:
        asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else "ws://10.10.10.10/ws"))
    except KeyboardInterrupt:
        pass