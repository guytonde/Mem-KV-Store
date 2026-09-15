#pragma once
#include <atomic>
#include <cstdint>

#include "futex.hpp"

namespace kvstore {
namespace util {

// Writer-preferring reader/writer lock
// All state in a 32-bit word, so it updates with a single compare-exchange
//
//   bit  0      writer active
//   bits 1..15  active readers  (max 32767)
//   bits 16..30 waiting writers (max 32767)
//
// Both counters saturate so the 32768th reader waits for one to leave and the
// 32768th queued writer waits for one to get in, instead of carrying into the
// neighbouring field.
//
// A reader that shows up while a writer is queued parks instead of joining the
// current batch. Without that a steady stream of readers starves writers
// forever on a read-heavy workload i think?
//
// Waiters park on sequence counters. Read the counter, recheck the state, then
// sleep on the counter's old value. A release landing in that window bumps the
// counter, so futex_wait sees the mismatch and returns instead of sleeping.
// Wakes are broadcast, so a thread that loses the race parks again.
class RWMutex {
public:
    constexpr RWMutex() noexcept = default;
    ~RWMutex() = default;

    RWMutex(const RWMutex&) = delete;
    RWMutex& operator=(const RWMutex&) = delete;
    RWMutex(RWMutex&&) = delete;
    RWMutex& operator=(RWMutex&&) = delete;

    // Read lock, yields to queued writers
    void lock_shared() noexcept {
        for (int spin = 0; spin < kSpinCount; ++spin) {
            if (try_lock_shared()) return;
            cpu_relax();
        }
        for (;;) {
            uint32_t state = state_.load(std::memory_order_relaxed);
            if (readers_may_enter(state)) {
                if (state_.compare_exchange_weak(state, state + kReaderUnit,
                                                 std::memory_order_acquire,
                                                 std::memory_order_relaxed)) {
                    return;
                }
                continue;
            }
            // Sequence is read before the final check, so a release in
            // between cancels the sleep rather than getting lost.
            const uint32_t seq = reader_seq_.load(std::memory_order_acquire);
            if (readers_may_enter(state_.load(std::memory_order_relaxed))) continue;
            futex_wait(&reader_seq_, seq);
        }
    }

    bool try_lock_shared() noexcept {
        uint32_t state = state_.load(std::memory_order_relaxed);
        if (!readers_may_enter(state)) return false;
        return state_.compare_exchange_weak(state, state + kReaderUnit,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed);
    }

    void unlock_shared() noexcept {
        const uint32_t previous = state_.fetch_sub(kReaderUnit, std::memory_order_release);
        // Last reader out is responsible for releasing any queued writer.
        if (readers_of(previous) == 1 && waiting_writers_of(previous) > 0) wake_writers();
        else if (readers_of(previous) == kMaxCount) wake_readers();
    }

    // write lock registers as a waiter first which blocks new readers
    void lock() noexcept {
        uint32_t expected = 0;
        if (state_.compare_exchange_strong(expected, kWriterActive, std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
            return;
        }
        register_waiting_writer();
        int spin = 0;
        for (;;) {
            uint32_t state = state_.load(std::memory_order_relaxed);
            if (writer_may_enter(state)) {
                const uint32_t next = (state - kWaitingWriterUnit) | kWriterActive;
                if (state_.compare_exchange_weak(state, next, std::memory_order_acquire,
                                                 std::memory_order_relaxed)) {
                    return;
                }
                continue;
            }
            if (spin++ < kSpinCount) {
                cpu_relax();
                continue;
            }
            const uint32_t seq = writer_seq_.load(std::memory_order_acquire);
            if (writer_may_enter(state_.load(std::memory_order_relaxed))) continue;
            futex_wait(&writer_seq_, seq);
        }
    }

    bool try_lock() noexcept {
        uint32_t state = state_.load(std::memory_order_relaxed);
        if ((state & kWriterActive) != 0 || readers_of(state) != 0) return false;
        return state_.compare_exchange_weak(state, state | kWriterActive,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed);
    }

    void unlock() noexcept {
        state_.fetch_and(~kWriterActive, std::memory_order_release);
        // Hand off to a queued writer if there is one; else release the reader batch
        if (waiting_writers_of(state_.load(std::memory_order_relaxed)) > 0) wake_writers();
        else wake_readers();
    }

private:
    static constexpr uint32_t kMaxCount = 0x7FFFu;
    static constexpr uint32_t kWriterActive = 1u;
    static constexpr uint32_t kReaderUnit = 1u << 1;
    static constexpr uint32_t kReaderMask = kMaxCount << 1;
    static constexpr uint32_t kWaitingWriterUnit = 1u << 16;
    static constexpr uint32_t kWaitingWriterMask = kMaxCount << 16;

    static uint32_t readers_of(uint32_t state) noexcept { return (state & kReaderMask) >> 1; }
    static uint32_t waiting_writers_of(uint32_t state) noexcept {
        return (state & kWaitingWriterMask) >> 16;
    }
    static bool readers_may_enter(uint32_t state) noexcept {
        return (state & kWriterActive) == 0 && waiting_writers_of(state) == 0 &&
               readers_of(state) < kMaxCount;
    }
    static bool writer_may_enter(uint32_t state) noexcept {
        return (state & kWriterActive) == 0 && readers_of(state) == 0;
    }

    // Bump the waiting count or park if its full
    void register_waiting_writer() noexcept {
        uint32_t state = state_.load(std::memory_order_relaxed);
        for (;;) {
            if (waiting_writers_of(state) < kMaxCount) {
                if (state_.compare_exchange_weak(state, state + kWaitingWriterUnit,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
                    return;
                }
                continue;
            }
            const uint32_t seq = writer_seq_.load(std::memory_order_acquire);
            state = state_.load(std::memory_order_relaxed);
            if (waiting_writers_of(state) < kMaxCount) continue;
            futex_wait(&writer_seq_, seq);
            state = state_.load(std::memory_order_relaxed);
        }
    }

    void wake_readers() noexcept {
        reader_seq_.fetch_add(1, std::memory_order_release);
        futex_wake_all(&reader_seq_);
    }

    void wake_writers() noexcept {
        writer_seq_.fetch_add(1, std::memory_order_release);
        futex_wake_all(&writer_seq_);
    }

    std::atomic<uint32_t> state_{0};
    std::atomic<uint32_t> reader_seq_{0};
    std::atomic<uint32_t> writer_seq_{0};
};

class SharedLock {
public:
    explicit SharedLock(RWMutex& mutex) noexcept : mutex_(mutex) { mutex_.lock_shared(); }
    ~SharedLock() { mutex_.unlock_shared(); }

    SharedLock(const SharedLock&) = delete;
    SharedLock& operator=(const SharedLock&) = delete;
    SharedLock(SharedLock&&) = delete;
    SharedLock& operator=(SharedLock&&) = delete;

private:
    RWMutex& mutex_;
};

class ExclusiveLock {
public:
    explicit ExclusiveLock(RWMutex& mutex) noexcept : mutex_(mutex) { mutex_.lock(); }
    ~ExclusiveLock() { mutex_.unlock(); }

    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;
    ExclusiveLock(ExclusiveLock&&) = delete;
    ExclusiveLock& operator=(ExclusiveLock&&) = delete;

private:
    RWMutex& mutex_;
};

} // namespace util
} // namespace kvstore