# STEP-08 — Верификация и финализация

> Статус: **done** — чистая сборка зелёная, `sdkconfig.defaults` создан,
> README/AGENTS.md обновлены, планы отмечены done. Проверка на железе (WS-тест)
> отложена до появления платы.
> Контекст: свежая сессия. STEP-07 выполнен: проект собирается на новой
> архитектуре (BootManager, слои 01..05, старые компоненты удалены). Этот шаг —
> проверка на железе, фиксация конфигурации сборки и обновление документации.
> Проверка: `idf.py build`, прошивка, монитор, WS-тест.

## Цель

1. Убедиться, что прошивка работает: AP поднимается, WS принимает клиентов,
   J1939-снапшоты публикуются.
2. Зафиксировать дефолты конфигурации (`sdkconfig.defaults`).
3. Обновить README, согласовать AGENTS.md со статусом миграции.

## Шаг 1 — Чистая сборка и прошивка

```bash
idf.py clean
idf.py reconfigure
idf.py build
idf.py -p <COMx> flash monitor
```

Ожидаемый лог старта:
- `AppContext: Event loop created successfully`;
- `ConfigStore` загрузил/создал `/spiffs/config.json`;
- `WifiApModule`: softAP `J1939_AP` started;
- `ServerModule`: HTTP server started; file server started;
- `J1939System`: TWAI driver installed/started; при активной CAN-шине — логи
  приёма кадров (на DEBUG);
- `CommunicationModule ready`.

## Шаг 2 — Проверка WebSocket

Браузер/инструмент подключается к `ws://10.10.10.10/ws` (после подключения к
AP `J1939_AP`). Без нового фронтенда проверку делаем утилитой. Варианты:

- **websocat**: `websocat -b ws://10.10.10.10/ws` — должны приходить бинарные
  кадры (байты `5A A5 01 20 01 00 ...`);
- **python** (если есть `websockets`): простой скрипт-читалка в `scripts/`
  (создать в этом шаге, см. ниже);
- **node**: `node -e "..."` — см. ниже.

В мониторе прошивки на подключение клиента:
`WsHandler: Client N added, total: 1` и `Comm: WS client N connected`.
Периодически (раз в ~250 мс) — отправка снапшота (лог на DEBUG в CommModule).

### Проверка формата кадра (руками)

Первый байты кадра снапшота:
```
5A A5 01 20 01 00 <len lo> <len hi> <seq lo> <seq hi> <count> <sa> <pgn 3b LE> <len> <data...> <periodMs 2b LE> ... <crc lo> <crc hi>
```
Проверить: `[0..1]=5A A5`, `[2]=01`, `[3]=0x20` (SNAPSHOT), `[4..5]=01 00`
(MsgType=J1939_SNAPSHOT). CRC на последних 2 байтах — пересчитать `crc16` по
всему кадру без CRC (та же функция, что в `J1939Proto::crc16`).

## Шаг 3 — Тестовый скрипт (опционально)

Создать `scripts/ws_snapshot_check.py` (простой клиент на `websockets`):

```python
import asyncio, websockets

async def main():
    async with websockets.connect("ws://10.10.10.10/ws", max_size=2**20) as ws:
        for _ in range(20):
            msg = await asyncio.wait_for(ws.recv(), timeout=5)
            if not isinstance(msg, bytes):
                continue
            print(f"frame {len(msg)}B magic={msg[0]:02X}{msg[1]:02X} "
                  f"ver={msg[2]} flags={msg[3]:02X} type={int.from_bytes(msg[4:6],'little')} "
                  f"payload={int.from_bytes(msg[6:8],'little')} seq={int.from_bytes(msg[8:10],'little')} "
                  f"records={msg[10] if len(msg) > 10 else '?'}")

asyncio.run(main())
```

Запуск: `python scripts/ws_snapshot_check.py` (клиент подключён к AP).

## Шаг 4 — `sdkconfig.defaults`

Создать `sdkconfig.defaults` с ключевыми дефолтами (остальное — как в текущем
`sdkconfig`):

```
CONFIG_IDF_TARGET="esp32"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_new.csv"
CONFIG_ESP_TASK_WDT_PANIC=y
```

(Сверить имена опций с фактическим `sdkconfig` перед записью — точечно через
`grep`, не читать весь файл.)

## Шаг 5 — README.md

Обновить `README.md`: описать новую архитектуру (слои, event bus, BootManager,
протокол — ссылка на `docs/PROTOCOL-J1939.md`), команды сборки, порты/пины,
как проверить. Убрать устаревшее про «пример twai» — это теперь полноценный
сканер J1939 с бекендом по TEMP_PID-архитектуре.

## Шаг 6 — AGENTS.md

Сверить с фактическим состоянием: структура (слои 01..05), команды сборки,
соглашения, статус «миграция завершена». Если в ходе шагов что-то уехало от
плана — поправить и в `docs/PLAN/*`, и в AGENTS.md.

## Шаг 7 — Замечания/открытые хвосты

- Фронтенд (`spiffs_image/`) пока **старый** и не понимает новый протокол
  (MsgType/батч). Это следующий этап — пересборка из `vue3_embedded` под
  `docs/PROTOCOL-J1939.md`. Не пытаться чинить фронт в этом шаге.
- Если период снапшота по умолчанию 250 мс окажется частым для CAN-шины с
  большим числом PGN — поднять `snapshotIntervalMs` в конфиге.
- Деплой без железа: на этом шаге проверка только сборкой; тест на WS — при
  наличии платы.

## Критерий готовности

1. `idf.py build` — чистый успех.
2. При наличии железа: AP поднят, WS-клиент получает корректные кадры
   (magic/ver/flags/type совпадают со спецификацией).
3. `sdkconfig.defaults`, обновлённые README и AGENTS.md зафиксированы.
4. Планы `docs/PLAN/*` отмечены выполненными (заголовок «Статус: done») или
   скорректированы по факту.