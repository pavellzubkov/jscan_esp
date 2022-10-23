# jscan_esp
Example of using esp32 twai for J1939

**Описание**

Пример использования esp32 twai (can) для чтения J1939 сообщений.
Поддержка приема широковещательного транспортного протокола. Передача по websocket в браузер. [Веб интерфейс](https://github.com/pavellzubkov/vue3_embedded)

**Сборка**

Для сборки - установить расширение EspressifIDF для VSCode использованная версия ESP-IDF - 4.3.2
Перед сборкой выполнить idf.py reconfigure для инициализации системы сборки.
[Веб интерфейс](https://github.com/pavellzubkov/vue3_embedded) в папке spiffs_image

**Дополнительно**

- Адрес узла в J1939 сети -25
- SSID: J1939_AP
- AP_PASS: 12345678
- ADR: http://10.10.10.10

![Alt text](docs/circuit.png?raw=true "Optional Title")


