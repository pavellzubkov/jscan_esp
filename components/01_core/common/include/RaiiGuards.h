#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <utility>

// RAII-примитивы захвата ресурсов: C++ сам заботится об освобождении.
// Header-only, без AppContext/событий — можно тянуть в любые слои,
// включая «глупые» драйверы 02.

// Захват FreeRTOS-семафора/мьютекса на время жизни объекта.
// Конструктор — Take, деструктор — Give: ресурс возвращается на ВСЕХ
// путях выхода (return/исключение/ранний break), без ручных Give.
//
// Не копируется (двойной Give), перенос = освобождение источника.
class LockGuard {
public:
    // timeout = portMAX_DELAY (по умолчанию) — классический мьютекс.
    // С меньшим таймаутом — «попытка захвата»: проверяйте owns().
    explicit LockGuard(SemaphoreHandle_t m,
                       TickType_t timeout = portMAX_DELAY)
        : m_(m)
    {
        if (m_ != nullptr)
            locked_ = (xSemaphoreTake(m_, timeout) == pdTRUE);
    }

    ~LockGuard() { unlock(); }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

    LockGuard(LockGuard&& other) noexcept
        : m_(other.m_), locked_(other.locked_)
    {
        other.m_ = nullptr;
        other.locked_ = false;
    }
    LockGuard& operator=(LockGuard&& other) noexcept
    {
        if (this != &other)
        {
            unlock();               // вернуть текущий ресурс
            m_ = other.m_;
            locked_ = other.locked_;
            other.m_ = nullptr;
            other.locked_ = false;
        }
        return *this;
    }

    // true, если Take прошёл (ресурс реально захвачен).
    bool owns() const { return locked_; }

    // Досрочный возврат ресурса (деструктор станет no-op).
    void unlock()
    {
        if (locked_ && m_ != nullptr)
            xSemaphoreGive(m_);
        locked_ = false;
    }

private:
    SemaphoreHandle_t m_ = nullptr;
    bool locked_ = false;
};

// Универсальный scope-guard: выполняет фунktor один раз при выходе из
// области видимости (для отката в begin(), освобождения на error-path).
// Отключается вручную через dismiss(), если инициализация удалась.
//
//   ScopeGuard rollback([&] { stop(); });
//   ... error → return;   // stop() вызовется автоматически
//   rollback.dismiss();   // успех — откат не нужен
template <typename F>
class ScopeGuard {
public:
    explicit ScopeGuard(F&& fn) : fn_(std::move(fn)), active_(true) {}

    ~ScopeGuard()
    {
        if (active_)
            fn_();
    }

    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ScopeGuard(ScopeGuard&&) = delete;
    ScopeGuard& operator=(ScopeGuard&&) = delete;

    // Инициализация удалась — откат не нужен.
    void dismiss() { active_ = false; }

private:
    F fn_;
    bool active_;
};

// Шаблонный deduction guide: ScopeGuard(fn) → ScopeGuard<F>.
template <typename F>
ScopeGuard(F) -> ScopeGuard<F>;
