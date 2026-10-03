#pragma once
// Заглушка freertos/FreeRTOS.h для host-тестов: типы хэндлов и базовые макросы,
// которых требуют заголовки компонентов (TwaiDriver.h и др.).
// Тиков/задач нет — тайминги в тестах передаются параметрами (nowMs).
#include <cstdint>

typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef unsigned UBaseType_t;

typedef void* QueueHandle_t;
typedef void* SemaphoreHandle_t;
typedef void* TaskHandle_t;

#define pdMS_TO_TICKS(ms)   ((TickType_t)(ms))
#define pdTRUE              1
#define pdFALSE             0
#define pdPASS              1
#define pdFAIL              0
#define portMAX_DELAY       ((TickType_t)0xFFFFFFFFu)