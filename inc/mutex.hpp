#pragma once
#include <atomic>
#include <cstdint>

#include "futex.hpp"

namespace kvstore {
namespace util {

// Futex mutex with three states
//   0 = unlocked, 1 = locked, 2 = locked and someone is parked
// unlock() only syscalls when the word is 2 so an uncontended lock/unlock pair
// is one compare-exchange and one store
class Mutex {
public:
    constexpr Mutex() noexcept = default;
    ~Mutex() = default;

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() noexcept {
        uint32_t expected = kUnlocked;
        if (state_.compare_exchange_strong(expected, kLocked, std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
            return;
        }
        lock_contended();
    }

    bool try_lock() noexcept {
        uint32_t expected = kUnlocked;
        return state_.compare_exchange_strong(expected, kLocked, std::memory_order_acquire,
                                              std::memory_order_relaxed);
    }

    void unlock() noexcept {
        // drop from kLocked means nobody is parked; skip  syscall
        if (state_.fetch_sub(1, std::memory_order_release) != kLocked) {
            state_.store(kUnlocked, std::memory_order_release);
            futex_wake(&state_, 1);
        }
    }

private:
    void lock_contended() noexcept {
        for (int i = 0; i < kSpinCount; ++i) {
            uint32_t expected = kUnlocked;
            if (state_.compare_exchange_weak(expected, kLocked, std::memory_order_acquire,
                                             std::memory_order_relaxed)) {
                return;
            }
            cpu_relax();
        }
        // Claim the word as contended then park until handed over
        uint32_t previous = state_.exchange(kContended, std::memory_order_acquire);
        while (previous != kUnlocked) {
            futex_wait(&state_, kContended);
            previous = state_.exchange(kContended, std::memory_order_acquire);
        }
    }

    static constexpr uint32_t kUnlocked = 0;
    static constexpr uint32_t kLocked = 1;
    static constexpr uint32_t kContended = 2;

    std::atomic<uint32_t> state_{kUnlocked};
};

class LockGuard {
public:
    explicit LockGuard(Mutex& mutex) noexcept : mutex_(mutex) { mutex_.lock(); }
    ~LockGuard() { mutex_.unlock(); }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Mutex& mutex_;
};

} // namespace util
} // namespace kvstore
