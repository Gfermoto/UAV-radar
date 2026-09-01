/**
 * @file    TcpBudget.h
 * @brief   Счётчик активных TCP-слотов для защиты пула lwIP PCB.
 *
 * Лимит `MAX_TCP_CONNECTIONS` (28) — runtime-бюджет под RTSP accept и прочий
 * TCP; при исчерпании `tryAcquire()` → false (отклонить новое соединение).
 * `release()` идемпотентен (не уходит в минус при double-release).
 *
 * Потокобезопасность: `std::atomic` CAS. Только static-методы.
 *
 * @see RTSPServer (accept path), docs/ARCHITECTURE.md
 */

#ifndef TCP_BUDGET_H
#define TCP_BUDGET_H

#include <atomic>
#include <cstdint>

class TcpBudget {
public:
    static constexpr int32_t MAX_TCP_CONNECTIONS = 28;

    /// Зарезервировать слот. @return false если пул исчерпан.
    static bool tryAcquire() {
        int32_t expected = _count.load(std::memory_order_relaxed);
        while (expected < MAX_TCP_CONNECTIONS) {
            if (_count.compare_exchange_weak(expected, expected + 1,
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    /// Освободить слот (не уходит ниже 0).
    static void release() {
        int32_t prev = _count.fetch_sub(1, std::memory_order_release);
        if (prev <= 0) {
            _count.fetch_add(1, std::memory_order_release);
        }
    }

    static int32_t activeCount() {
        return _count.load(std::memory_order_relaxed);
    }

    TcpBudget()  = delete;
    ~TcpBudget() = delete;

private:
    inline static std::atomic<int32_t> _count{0};
};

#endif  // TCP_BUDGET_H
