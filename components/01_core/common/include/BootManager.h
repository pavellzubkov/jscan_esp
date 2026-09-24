#pragma once
#include "esp_err.h"
#include <cstddef>
#include <cstdint>
#include <utility>

class AppContext;

// Единая точка создания модуля: new -> begin -> delete при ошибке.
// Объявлен здесь, чтобы main оставался декларативной таблицей модулей.
template <typename Module, typename... Args>
inline esp_err_t makeModule(AppContext* ctx, Args&&... args) {
    Module* m = new Module(ctx, std::forward<Args>(args)...);
    esp_err_t err = m->begin();
    if (err != ESP_OK) { delete m; }
    return err;
}

// Регистрация модуля в BootManager. Критичные модули при ошибке останавливают
// загрузку; некритичные — логируют ошибку, но система продолжает старт.
#define REGISTER_MODULE(boot, name, Type, priority, critical, ...) \
    (boot).add({name, [](AppContext* c) { return makeModule<Type>(c, ##__VA_ARGS__); }, priority, critical})

/**
 * @brief BootManager — табличный загрузчик модулей.
 *
 * Позволяет зарегистрировать модули с приоритетом и флагом critical,
 * после чего запускает их в порядке приоритета.
 * Критичные модули при ошибке останавливают загрузку;
 * некритичные — логируют ошибку, но система продолжает старт.
 */
class BootManager {
public:
    static constexpr size_t kMaxModules = 16;

    struct Entry {
        const char* name;                           ///< Имя модуля (для логов и isReady)
        esp_err_t (*init)(AppContext* ctx);          ///< Функция инициализации (создание + begin)
        uint8_t priority;                            ///< 0 = первый, 255 = последний
        bool critical;                               ///< true = без него система не работает
    };

    /** Зарегистрировать модуль. */
    esp_err_t add(const Entry& entry);

    /**
     * @brief Запустить все модули в порядке приоритета.
     * @return ESP_OK, или код первой ошибки (продолжает запуск некритичных).
     *         Если critical-модуль упал — возвращает его ошибку и останавливается.
     */
    esp_err_t startAll(AppContext* ctx);

    /** Вывести в лог статус всех модулей. */
    void dumpStatus() const;

    /** Проверить, успешно ли проинициализирован модуль. */
    bool isReady(const char* name) const;

private:
    Entry entries_[kMaxModules];
    esp_err_t results_[kMaxModules];
    bool started_[kMaxModules] = {false};
    size_t count_ = 0;

    static void sortByPriority(Entry* entries, size_t count);
};
