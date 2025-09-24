#pragma once
#include <shared_mutex>
#include <atomic>
#include <chrono>

namespace kvstore {
namespace util {

/**
 * Reader-writer mutex wrapper that prevents writer starvation.
 * Uses std::shared_mutex with additional fairness mechanisms.
 */
class RWMutex {
public:
    RWMutex() = default;
    ~RWMutex() = default;
    
    // Non-copyable, non-movable
    RWMutex(const RWMutex&) = delete;
    RWMutex& operator=(const RWMutex&) = delete;
    RWMutex(RWMutex&&) = delete;
    RWMutex& operator=(RWMutex&&) = delete;
    
    /**
     * Acquire shared (read) lock
     * Blocks if writers are waiting to prevent starvation
     */
    void lock_shared();
    
    /**
     * Try to acquire shared (read) lock without blocking
     * @return true if lock acquired, false otherwise
     */
    bool try_lock_shared();
    
    /**
     * Release shared (read) lock
     */
    void unlock_shared();
    
    /**
     * Acquire exclusive (write) lock
     * Prioritized over readers to prevent starvation
     */
    void lock();
    
    /**
     * Try to acquire exclusive (write) lock without blocking
     * @return true if lock acquired, false otherwise
     */
    bool try_lock();
    
    /**
     * Release exclusive (write) lock
     */
    void unlock();

private:
    mutable std::shared_mutex mutex_;
    
    // Writer starvation prevention
    mutable std::atomic<int> waiting_writers_{0};
    mutable std::atomic<int> active_readers_{0};
    
    // Fairness control
    static constexpr int MAX_READERS_BEFORE_WRITER = 100;
    mutable std::atomic<int> consecutive_readers_{0};
};

/**
 * RAII shared lock guard for RWMutex
 */
class SharedLock {
public:
    explicit SharedLock(RWMutex& mutex) : mutex_(mutex) {
        mutex_.lock_shared();
    }
    
    ~SharedLock() {
        mutex_.unlock_shared();
    }
    
    // Non-copyable, non-movable
    SharedLock(const SharedLock&) = delete;
    SharedLock& operator=(const SharedLock&) = delete;
    SharedLock(SharedLock&&) = delete;
    SharedLock& operator=(SharedLock&&) = delete;

private:
    RWMutex& mutex_;
};

/**
 * RAII exclusive lock guard for RWMutex
 */
class ExclusiveLock {
public:
    explicit ExclusiveLock(RWMutex& mutex) : mutex_(mutex) {
        mutex_.lock();
    }
    
    ~ExclusiveLock() {
        mutex_.unlock();
    }
    
    // Non-copyable, non-movable
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;
    ExclusiveLock(ExclusiveLock&&) = delete;
    ExclusiveLock& operator=(ExclusiveLock&&) = delete;

private:
    RWMutex& mutex_;
};

} // namespace util
} // namespace kvstore