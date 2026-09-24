#pragma once
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <cstddef>
#include <cstdint>
#include <type_traits>

// Намеренные касты между указателями на методы (обобщённое хранение в пуле).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"

// Универсальный менеджер событий: подписка обычных C++ методов классов и
// свободных функций на любую базу событий (без макросов), публикация событий
// с автоматическим подсчётом размера структур данных.
//
// Подписки хранятся во встроенном пуле фиксированной вместимости — без
// heap-аллокаций на каждую подписку. Обработчики получают указатель на элемент
// пула, поэтому пул не перераспределяется. RAII-очистка: ~EventManager()
// (или shutdown()) снимает все подписки.
class EventManager {
private:
    esp_event_loop_handle_t event_loop_;
    // Вместимость пула подписок. Текущие модули занимают ~30 слотов;
    // запас оставлен под добавление новых модулей/транспортов. При
    // исчерпании подписка отклоняется с ESP_LOGE (не тихо).
    static constexpr size_t kMaxSubscriptions = 64;
    // Таймаут публикации: не блокироваться на полной очереди событий
    // (например, при вызове из обработчика event-loop). 50 мс достаточно,
    // чтобы дождаться слота при кратковременном всплеске, но не грозит
    // вечным висением/взаимоблокировкой.
    static constexpr TickType_t kPostTimeoutTicks = pdMS_TO_TICKS(50);
    static inline const char* TAG = "EventManager";

    // Класс-заглушка для хранения указателя на метод в обобщённом виде
    // (все указатели на методы имеют одинаковое представление на ESP32).
    struct MethodSlot;
    typedef void (MethodSlot::*MethodPtr)();

    // Запись о подписке. handler_args обработчика указывает на элемент пула,
    // поэтому пул фиксированной вместимости и не растёт.
    struct Subscription {
        esp_event_base_t base;
        int32_t id;
        esp_event_handler_instance_t instance;
        esp_event_loop_handle_t loop;  // nullptr = системный (default) loop
        void* obj;
        MethodPtr method;
    };

    Subscription subscriptions_[kMaxSubscriptions];
    size_t subscriptionCount_ = 0;

    // Выделить слот в пуле (без продвижения счётчика). nullptr, если пул полон.
    Subscription* reserveSubscription() {
        if (subscriptionCount_ >= kMaxSubscriptions) {
            return nullptr;
        }
        Subscription* s = &subscriptions_[subscriptionCount_];
        s->base = nullptr;
        s->id = 0;
        s->instance = nullptr;
        s->loop = nullptr;
        s->obj = nullptr;
        s->method = nullptr;
        return s;
    }

public:
    EventManager() : event_loop_(nullptr) {}

    void setEventLoop(esp_event_loop_handle_t loop) { event_loop_ = loop; }

    // Подписка: метод класса БЕЗ данных (тип возврата не важен)
    template <typename ObjType, typename EventEnum, typename ReturnType>
    bool subscribe(esp_event_base_t event_base, EventEnum event_id,
                   ReturnType (ObjType::*method)(), ObjType* obj) {
        if (!event_loop_ || !obj || !method) return false;
        int32_t int_event_id = static_cast<int32_t>(event_id);

        Subscription* sub = reserveSubscription();
        if (!sub) {
            ESP_LOGE(TAG, "Subscription pool exhausted");
            return false;
        }
        sub->base = event_base;
        sub->id = int_event_id;
        sub->loop = event_loop_;
        sub->obj = obj;
        sub->method = reinterpret_cast<MethodPtr>(method);

        auto c_callback = [](void* handler_args, esp_event_base_t, int32_t, void*) {
            auto* s = static_cast<Subscription*>(handler_args);
            auto* o = static_cast<ObjType*>(s->obj);
            (o->*(reinterpret_cast<ReturnType (ObjType::*)()>(s->method)))();
        };

        esp_err_t err = esp_event_handler_instance_register_with(
            event_loop_, event_base, int_event_id, c_callback, sub,
            &sub->instance);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to subscribe to event %d: %s",
                     int_event_id, esp_err_to_name(err));
            return false;
        }
        subscriptionCount_++;
        return true;
    }

    // Подписка: метод класса С данными (const DataType*)
    template <typename ObjType, typename EventEnum, typename DataType,
              typename ReturnType>
    bool subscribe(esp_event_base_t event_base, EventEnum event_id,
                   ReturnType (ObjType::*method)(const DataType*),
                   ObjType* obj) {
        if (!event_loop_ || !obj || !method) return false;
        int32_t int_event_id = static_cast<int32_t>(event_id);

        Subscription* sub = reserveSubscription();
        if (!sub) {
            ESP_LOGE(TAG, "Subscription pool exhausted");
            return false;
        }
        sub->base = event_base;
        sub->id = int_event_id;
        sub->loop = event_loop_;
        sub->obj = obj;
        sub->method = reinterpret_cast<MethodPtr>(method);

        auto c_callback = [](void* handler_args, esp_event_base_t, int32_t,
                             void* event_data) {
            auto* s = static_cast<Subscription*>(handler_args);
            auto* o = static_cast<ObjType*>(s->obj);
            (o->*(reinterpret_cast<ReturnType (ObjType::*)(const DataType*)>(
                      s->method)))(static_cast<const DataType*>(event_data));
        };

        esp_err_t err = esp_event_handler_instance_register_with(
            event_loop_, event_base, int_event_id, c_callback, sub,
            &sub->instance);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to subscribe to event %d: %s",
                     int_event_id, esp_err_to_name(err));
            return false;
        }
        subscriptionCount_++;
        return true;
    }

    // Подписка: свободная функция / статический метод / лямбда без захвата
    // (на loop приложения)
    template <typename EventEnum>
    bool subscribe(esp_event_base_t event_base, EventEnum event_id,
                   esp_event_handler_t handler, void* arg = nullptr) {
        if (!event_loop_ || !handler) return false;

        Subscription* sub = reserveSubscription();
        if (!sub) {
            ESP_LOGE(TAG, "Subscription pool exhausted");
            return false;
        }
        sub->base = event_base;
        sub->id = static_cast<int32_t>(event_id);
        sub->loop = event_loop_;
        sub->obj = arg;

        esp_err_t err = esp_event_handler_instance_register_with(
            event_loop_, event_base, static_cast<int32_t>(event_id), handler,
            arg, &sub->instance);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to subscribe to event %d: %s",
                     event_id, esp_err_to_name(err));
            return false;
        }
        subscriptionCount_++;
        return true;
    }

    // Подписка на системные события (default loop): WIFI_EVENT/IP_EVENT и т.п.
    // Возвращает instance для ручного unregister (или nullptr при ошибке).
    esp_event_handler_instance_t subscribeDefault(esp_event_base_t event_base,
                                                  int32_t event_id,
                                                  esp_event_handler_t handler,
                                                  void* arg = nullptr) {
        if (!handler) return nullptr;

        Subscription* sub = reserveSubscription();
        if (!sub) {
            ESP_LOGE(TAG, "Subscription pool exhausted");
            return nullptr;
        }
        sub->base = event_base;
        sub->id = event_id;
        sub->loop = nullptr;  // default loop
        sub->obj = arg;

        esp_err_t err = esp_event_handler_instance_register(
            event_base, event_id, handler, arg, &sub->instance);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to subscribe to event %d: %s",
                     event_id, esp_err_to_name(err));
            return nullptr;
        }
        subscriptionCount_++;
        return sub->instance;
    }

    // Публикация события БЕЗ данных
    template <typename EventEnum>
    bool post(esp_event_base_t event_base, EventEnum event_id) {
        if (!event_loop_) {
            ESP_LOGE(TAG, "Event loop not initialized");
            return false;
        }
        esp_err_t err = esp_event_post_to(
            event_loop_, event_base, static_cast<int32_t>(event_id),
            nullptr, 0, kPostTimeoutTicks);
        if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Event queue full, dropped event %d",
                     static_cast<int32_t>(event_id));
            return false;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to post event %d: %s",
                     static_cast<int32_t>(event_id), esp_err_to_name(err));
            return false;
        }
        return true;
    }

    // Публикация события С данными (sizeof вычисляется автоматически)
    template <typename EventEnum, typename DataType>
    bool post(esp_event_base_t event_base, EventEnum event_id,
              const DataType& data) {
        static_assert(!std::is_pointer_v<DataType>,
                      "Передавайте структуры по ссылке, а не указатели!");
        if (!event_loop_) {
            ESP_LOGE(TAG, "Event loop not initialized");
            return false;
        }
        esp_err_t err = esp_event_post_to(
            event_loop_, event_base, static_cast<int32_t>(event_id),
            static_cast<const void*>(&data), sizeof(DataType), kPostTimeoutTicks);
        if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Event queue full, dropped event %d",
                     static_cast<int32_t>(event_id));
            return false;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to post event %d: %s",
                     static_cast<int32_t>(event_id), esp_err_to_name(err));
            return false;
        }
        return true;
    }

    // Публикация с явным размером (flexible array и т.п.)
    template <typename EventEnum>
    bool postSized(esp_event_base_t event_base, EventEnum event_id,
                   const void* data, size_t size) {
        if (!event_loop_) {
            ESP_LOGE(TAG, "Event loop not initialized");
            return false;
        }
        esp_err_t err = esp_event_post_to(event_loop_, event_base,
                                          static_cast<int32_t>(event_id),
                                          data, size, kPostTimeoutTicks);
        if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Event queue full, dropped event %d",
                     static_cast<int32_t>(event_id));
            return false;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to post event %d: %s",
                     event_id, esp_err_to_name(err));
            return false;
        }
        return true;
    }

    // Снять все подписки данного объекта (идемпотентно). Нужен для модулей,
    // которые удаляются после подписки (delete в makeModule при ошибке begin()):
    // иначе обработчики указывают на удалённый объект (use-after-free).
    void unsubscribe(void* obj) {
        if (!obj) return;
        for (size_t i = 0; i < subscriptionCount_;) {
            Subscription* s = &subscriptions_[i];
            if (s->obj != obj) {
                i++;
                continue;
            }
            if (s->instance) {
                if (s->loop) {
                    esp_event_handler_instance_unregister_with(
                        s->loop, s->base, s->id, s->instance);
                } else {
                    esp_event_handler_instance_unregister(s->base, s->id,
                                                          s->instance);
                }
            }
            // Заполняем дыру последним элементом пула
            if (i != subscriptionCount_ - 1) {
                subscriptions_[i] = subscriptions_[subscriptionCount_ - 1];
            }
            --subscriptionCount_;
            // i не инкрементируем: на место i переехал последний элемент
        }
    }

    // Снять все подписки (идемпотентно). Вызывается из ~EventManager и из
    // ~AppContext ДО esp_event_loop_delete, чтобы не работать с удалённым loop.
    void shutdown() {
        for (size_t i = 0; i < subscriptionCount_; i++) {
            Subscription* s = &subscriptions_[i];
            if (!s->instance) continue;
            if (s->loop) {
                esp_event_handler_instance_unregister_with(
                    s->loop, s->base, s->id, s->instance);
            } else {
                esp_event_handler_instance_unregister(s->base, s->id,
                                                      s->instance);
            }
            s->instance = nullptr;
        }
        subscriptionCount_ = 0;
    }

    ~EventManager() { shutdown(); }
};

#pragma GCC diagnostic pop
