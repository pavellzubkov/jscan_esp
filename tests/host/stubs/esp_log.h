#pragma once
// Заглушка esp_log для host-тестов (без IDF): логирование отключается.
#define ESP_LOGW(tag, ...) ((void)0)
#define ESP_LOGE(tag, ...) ((void)0)
#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGD(tag, ...) ((void)0)