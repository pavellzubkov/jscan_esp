// Единственное определение event base APP_EVENTS_BASE (IDF-идиома):
// ESP_EVENT_DEFINE_BASE стоял в заголовке и давал своё определение в
// каждом TU (C++ const — внутренняя связь); esp_event сравнивает base по
// указателю, поэтому рассинхрон адресов молча ломал бы доставку событий.
#include "AppEvents.hpp"

ESP_EVENT_DEFINE_BASE(APP_EVENTS_BASE);
