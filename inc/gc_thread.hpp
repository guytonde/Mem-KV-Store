#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

namespace kvstore {

// Calls store.gc() on an interval until destroyed. Without it a long write
// workload keeps every superseded version forever. Reclamation stays off the
// write path on purpose cuz a writer shouldn't pay for another thread's garbage.
template <typename Store>
class GcThread {
public:
    explicit GcThread(Store& store, std::chrono::milliseconds interval = std::chrono::milliseconds(50))
        : store_(store), interval_(interval), thread_([this] { run(); }) {}

    ~GcThread() {
        stop_.store(true, std::memory_order_relaxed);
        if (thread_.joinable()) thread_.join();
    }

    GcThread(const GcThread&) = delete;
    GcThread& operator=(const GcThread&) = delete;

    // Entries reclaimed so far.
    std::size_t reclaimed() const { return reclaimed_.load(std::memory_order_relaxed); }

    std::size_t sweeps() const { return sweeps_.load(std::memory_order_relaxed); }

private:
    void run() {
        while (!stop_.load(std::memory_order_relaxed)) {
            reclaimed_.fetch_add(store_.gc(), std::memory_order_relaxed);
            sweeps_.fetch_add(1, std::memory_order_relaxed);
            // Sleep in slices so shutdown doesn't wait out a whole interval.
            for (auto slept = std::chrono::milliseconds(0);
                 slept < interval_ && !stop_.load(std::memory_order_relaxed);
                 slept += std::chrono::milliseconds(5)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }

    Store& store_;
    std::chrono::milliseconds interval_;
    std::atomic<bool> stop_{false};
    std::atomic<std::size_t> reclaimed_{0};
    std::atomic<std::size_t> sweeps_{0};
    std::thread thread_;
};

} // namespace kvstore
