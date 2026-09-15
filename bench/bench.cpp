// throughput across threads, read/write mixes, shard counts, snapshot cost
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "gc_thread.hpp"
#include "kv_store.hpp"

using Clock = std::chrono::steady_clock;
using Store = kvstore::KVStore<uint64_t, uint64_t>;

namespace {

constexpr uint64_t kKeyspace = 100000;
constexpr int kOpsPerThread = 200000;

// 3ns, 2.5kb of state per thread is fine
using Random = std::mt19937_64;

double run_mix(Store& store, int threads, int read_percent) {
    // without gc this measures chain growth insted of the store
    kvstore::GcThread<Store> collector(store, std::chrono::milliseconds(20));
    std::atomic<bool> go{false};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));

    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&store, &go, t, read_percent] {
            Random rng(0x9E3779B97F4A7C15ull * static_cast<uint64_t>(t + 1));
            while (!go.load(std::memory_order_acquire)) kvstore::util::cpu_relax();
            uint64_t sink = 0;
            for (int i = 0; i < kOpsPerThread; ++i) {
                const uint64_t roll = rng();
                const uint64_t key = (roll >> 8) % kKeyspace;
                if (static_cast<int>(roll % 100) < read_percent) {
                    auto value = store.get(key);
                    sink += value.value_or(0);
                } else {
                    store.put(key, roll);
                }
            }
            // keep the reads from geting optimized away
            if (sink == 0xFFFFFFFFFFFFFFFFull) std::printf(" ");
        });
    }

    const auto start = Clock::now();
    go.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return (static_cast<double>(threads) * kOpsPerThread) / seconds / 1e6;  // Mops/s
}

void fill(Store& store) {
    for (uint64_t key = 0; key < kKeyspace; ++key) store.put(key, key);
    store.gc();
}

} // namespace

int main() {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    std::printf("hardware_concurrency = %u, keyspace = %llu, %d ops/thread\n\n", hw,
                static_cast<unsigned long long>(kKeyspace), kOpsPerThread);

    std::printf("throughput in Mops/s by thread count\n");
    std::printf("%8s %14s %14s %14s\n", "threads", "95% read", "50% read", "100% write");
    for (unsigned threads = 1; threads <= hw; threads *= 2) {
        Store store(64);
        fill(store);
        const double read_heavy = run_mix(store, static_cast<int>(threads), 95);
        const double mixed = run_mix(store, static_cast<int>(threads), 50);
        const double write_only = run_mix(store, static_cast<int>(threads), 0);
        std::printf("%8u %14.2f %14.2f %14.2f\n", threads, read_heavy, mixed, write_only);
    }

    std::printf("\nshard count sensitivity (%u threads, 50%% read)\n", std::min(hw, 8u));
    for (std::size_t shards : {1u, 4u, 16u, 64u, 256u}) {
        Store store(shards);
        fill(store);
        const double rate = run_mix(store, static_cast<int>(std::min(hw, 8u)), 50);
        std::printf("%8zu shards %10.2f Mops/s\n", shards, rate);
    }

    std::printf("\nsnapshot cost vs store size\n");
    for (uint64_t size : {1000ull, 100000ull, 1000000ull}) {
        Store store(64);
        for (uint64_t key = 0; key < size; ++key) store.put(key, key);
        store.gc();
        const auto start = Clock::now();
        constexpr int kIterations = 100000;
        for (int i = 0; i < kIterations; ++i) {
            auto snap = store.snapshot();
            (void)snap;
        }
        const double nanos =
            std::chrono::duration<double, std::nano>(Clock::now() - start).count() / kIterations;
        std::printf("%9llu keys %10.0f ns per snapshot\n", static_cast<unsigned long long>(size),
                    nanos);
    }

    std::printf("\nsnapshot scan throughput (1 writer + N scanners over 10k keys)\n");
    {
        Store store(64);
        for (uint64_t key = 0; key < 10000; ++key) store.put(key, key);
        std::atomic<bool> stop{false};
        std::atomic<long> scans{0};
        std::thread writer([&] {
            uint64_t i = 0;
            while (!stop.load(std::memory_order_relaxed)) store.put(i++ % 10000, i);
        });
        std::vector<std::thread> scanners;
        for (unsigned t = 0; t < std::min(hw, 4u); ++t) {
            scanners.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    auto snap = store.snapshot();
                    uint64_t total = 0;
                    snap.for_each([&](uint64_t, uint64_t value) { total += value; });
                    if (total == 0xFFFFFFFFFFFFFFFFull) std::printf(" ");
                    scans.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        stop.store(true);
        writer.join();
        for (auto& thread : scanners) thread.join();
        std::printf("%9ld full snapshot scans per second while writes continue\n", scans.load());
    }

    return 0;
}
