// tests for the hand rolled sync primitives
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "mutex.hpp"
#include "rwmutex.hpp"
#include "test_framework.hpp"

using kvstore::util::ExclusiveLock;
using kvstore::util::LockGuard;
using kvstore::util::Mutex;
using kvstore::util::RWMutex;
using kvstore::util::SharedLock;

TEST(mutex_provides_mutual_exclusion) {
    Mutex mutex;
    long counter = 0;  // deliberatly not atomic, the lock is what makes this safe
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 20000; ++i) {
                LockGuard guard(mutex);
                ++counter;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    CHECK_EQ(counter, 8L * 20000);
}

TEST(mutex_try_lock_reflects_ownership) {
    Mutex mutex;
    CHECK(mutex.try_lock());
    CHECK(!mutex.try_lock());
    mutex.unlock();
    CHECK(mutex.try_lock());
    mutex.unlock();
}

TEST(rwmutex_excludes_writers_from_each_other) {
    RWMutex mutex;
    long counter = 0;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 20000; ++i) {
                ExclusiveLock lock(mutex);
                ++counter;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    CHECK_EQ(counter, 8L * 20000);
}

TEST(rwmutex_readers_never_overlap_a_writer) {
    RWMutex mutex;
    std::atomic<int> readers{0};
    std::atomic<int> writers{0};
    std::atomic<bool> violation{false};
    std::vector<std::thread> threads;

    for (int t = 0; t < 6; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 20000; ++i) {
                SharedLock lock(mutex);
                readers.fetch_add(1, std::memory_order_acq_rel);
                if (writers.load(std::memory_order_acquire) != 0) violation.store(true);
                readers.fetch_sub(1, std::memory_order_acq_rel);
            }
        });
    }
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 5000; ++i) {
                ExclusiveLock lock(mutex);
                const int active = writers.fetch_add(1, std::memory_order_acq_rel) + 1;
                if (active != 1 || readers.load(std::memory_order_acquire) != 0) {
                    violation.store(true);
                }
                writers.fetch_sub(1, std::memory_order_acq_rel);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    CHECK(!violation.load());
}

TEST(rwmutex_allows_concurrent_readers) {
    RWMutex mutex;
    std::atomic<int> inside{0};
    std::atomic<int> peak{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 5000; ++i) {
                SharedLock lock(mutex);
                const int now = inside.fetch_add(1, std::memory_order_acq_rel) + 1;
                int observed = peak.load(std::memory_order_relaxed);
                while (now > observed &&
                       !peak.compare_exchange_weak(observed, now, std::memory_order_relaxed)) {
                }
                std::this_thread::yield();
                inside.fetch_sub(1, std::memory_order_acq_rel);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    // a lock that serialized readers would never see more then one inside
    CHECK(peak.load() > 1);
}

TEST(rwmutex_saturates_at_the_reader_limit) {
    RWMutex mutex;
    constexpr int kMax = 32767;
    for (int i = 0; i < kMax; ++i) mutex.lock_shared();
    CHECK(!mutex.try_lock_shared());

    std::atomic<bool> entered{false};
    std::thread extra([&] {
        SharedLock lock(mutex);
        entered.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!entered.load());  // the cap holds it back instead of wrapping the count
    mutex.unlock_shared();
    extra.join();
    CHECK(entered.load());

    for (int i = 0; i < kMax - 1; ++i) mutex.unlock_shared();
    CHECK(mutex.try_lock());  // count went back to zero, no bits leaked into the writer field
    mutex.unlock();
}

TEST(rwmutex_does_not_starve_writers) {
    RWMutex mutex;
    std::atomic<bool> stop{false};
    std::atomic<long> writes{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 8; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                SharedLock lock(mutex);
            }
        });
    }
    std::thread writer([&] {
        for (int i = 0; i < 1000; ++i) {
            ExclusiveLock lock(mutex);
            writes.fetch_add(1, std::memory_order_relaxed);
        }
    });
    writer.join();  // hangs here if readers can starve the writer
    stop.store(true);
    for (auto& thread : readers) thread.join();
    CHECK_EQ(writes.load(), 1000L);
}
