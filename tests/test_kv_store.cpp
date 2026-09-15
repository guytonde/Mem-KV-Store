// store tests, single threaded semantics, snapshot isolation, concurency, gc
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "core.hpp"
#include "kv_store.hpp"
#include "test_framework.hpp"

using kvstore::KVStore;

namespace {
using StringStore = KVStore<std::string, std::string>;
using IntStore = KVStore<int, long>;
} // namespace

TEST(basic_put_get_erase) {
    StringStore store;
    CHECK(store.empty());
    CHECK(!store.get("missing").has_value());

    store.put("a", "1");
    store.put("b", "2");
    CHECK_EQ(store.get("a").value(), std::string("1"));
    CHECK_EQ(store.size(), 2u);

    store.put("a", "3");
    CHECK_EQ(store.get("a").value(), std::string("3"));
    CHECK_EQ(store.size(), 2u);

    CHECK(store.erase("a"));
    CHECK(!store.erase("a"));
    CHECK(!store.get("a").has_value());
    CHECK(!store.contains("a"));
    CHECK_EQ(store.size(), 1u);

    store.clear();
    CHECK(store.empty());
}

TEST(compare_and_put_is_atomic_on_a_key) {
    IntStore store;
    store.put(1, 10);
    CHECK(!store.compare_and_put(1, 99, 20));   // wrong expectation
    CHECK_EQ(store.get(1).value(), 10L);
    CHECK(store.compare_and_put(1, 10, 20));
    CHECK_EQ(store.get(1).value(), 20L);
    CHECK(!store.compare_and_put(2, 0, 1));     // absent key
}

TEST(snapshot_is_isolated_from_later_writes) {
    StringStore store;
    store.put("k", "v1");
    store.put("gone", "here");

    auto snap = store.snapshot();

    store.put("k", "v2");
    store.put("new", "value");
    store.erase("gone");

    CHECK_EQ(snap.get("k").value(), std::string("v1"));
    CHECK(!snap.get("new").has_value());
    CHECK_EQ(snap.get("gone").value(), std::string("here"));  // delete is invisble to the snapshot
    CHECK_EQ(snap.size(), 2u);

    CHECK_EQ(store.get("k").value(), std::string("v2"));
    CHECK_EQ(store.size(), 2u);
}

TEST(snapshots_stack_at_distinct_versions) {
    IntStore store;
    store.put(1, 100);
    auto first = store.snapshot();
    store.put(1, 200);
    auto second = store.snapshot();
    store.put(1, 300);
    auto third = store.snapshot();

    CHECK_EQ(first.get(1).value(), 100L);
    CHECK_EQ(second.get(1).value(), 200L);
    CHECK_EQ(third.get(1).value(), 300L);
    CHECK(first.version() < second.version());
    CHECK(second.version() < third.version());
}

TEST(snapshot_outlives_the_store) {
    std::optional<kvstore::Snapshot<int, long>> snap;
    {
        IntStore store;
        store.put(7, 42);
        snap = store.snapshot();
    }
    CHECK_EQ(snap->get(7).value(), 42L);
}

TEST(snapshot_iteration_matches_contents) {
    IntStore store;
    for (int i = 0; i < 50; ++i) store.put(i, i * 2);
    auto snap = store.snapshot();
    for (int i = 0; i < 50; i += 2) store.erase(i);

    long sum = 0;
    std::size_t count = 0;
    snap.for_each([&](int key, long value) {
        CHECK_EQ(value, static_cast<long>(key) * 2);
        sum += value;
        ++count;
    });
    CHECK_EQ(count, 50u);
    CHECK_EQ(sum, 2450L);
    CHECK_EQ(snap.items().size(), 50u);
    CHECK_EQ(store.size(), 25u);
}

TEST(put_all_is_visible_atomically) {
    IntStore store;
    store.put_all({{1, 0}, {2, 0}});
    auto before = store.snapshot();
    store.put_all({{1, 5}, {2, 5}});
    auto after = store.snapshot();

    CHECK_EQ(before.get(1).value(), 0L);
    CHECK_EQ(before.get(2).value(), 0L);
    CHECK_EQ(after.get(1).value(), 5L);
    CHECK_EQ(after.get(2).value(), 5L);
}

TEST(concurrent_writers_on_distinct_keys) {
    IntStore store(16);
    constexpr int kThreads = 8;
    constexpr int kPerThread = 5000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&store, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const int key = t * kPerThread + i;
                store.put(key, static_cast<long>(key) + 1);
            }
        });
    }
    for (auto& thread : threads) thread.join();

    CHECK_EQ(store.size(), static_cast<std::size_t>(kThreads * kPerThread));
    for (int key = 0; key < kThreads * kPerThread; ++key) {
        CHECK_EQ(store.get(key).value(), static_cast<long>(key) + 1);
    }
    CHECK(store.version() >= static_cast<uint64_t>(kThreads) * kPerThread);
}

TEST(concurrent_writers_on_one_key_leave_a_valid_value) {
    IntStore store;
    constexpr int kThreads = 8;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&store, t] {
            for (int i = 0; i < 2000; ++i) store.put(0, t);
        });
    }
    for (auto& thread : threads) thread.join();

    const long final_value = store.get(0).value();
    CHECK(final_value >= 0 && final_value < kThreads);
}

TEST(readers_and_writers_run_together) {
    IntStore store(32);
    constexpr int kKeys = 500;
    for (int i = 0; i < kKeys; ++i) store.put(i, 0);

    std::atomic<bool> stop{false};
    std::atomic<bool> corrupt{false};
    std::atomic<long> reads{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            long round = 1;
            while (!stop.load(std::memory_order_relaxed)) {
                for (int i = 0; i < kKeys; ++i) store.put(i, round);
                ++round;
            }
        });
    }
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                for (int i = 0; i < kKeys; ++i) {
                    auto value = store.get(i);
                    if (!value.has_value() || *value < 0) corrupt.store(true);
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true);
    for (auto& thread : threads) thread.join();

    CHECK(!corrupt.load());
    CHECK(reads.load() > 0);
    CHECK_EQ(store.size(), static_cast<std::size_t>(kKeys));
}

TEST(snapshots_never_observe_a_partial_batch) {
    // money moves in one atomic batch so every snapshot sees the same total
    IntStore store(8);
    constexpr int kAccounts = 16;
    constexpr long kStart = 1000;
    for (int i = 0; i < kAccounts; ++i) store.put(i, kStart);
    const long expected_total = kAccounts * kStart;

    std::atomic<bool> stop{false};
    std::atomic<bool> broken{false};
    std::atomic<long> observations{0};

    std::thread transfers([&] {
        unsigned seed = 12345;
        while (!stop.load(std::memory_order_relaxed)) {
            seed = seed * 1103515245u + 12345u;
            const int from = static_cast<int>((seed >> 16) % kAccounts);
            const int to = (from + 1) % kAccounts;
            const long from_balance = store.get(from).value();
            const long to_balance = store.get(to).value();
            if (from_balance <= 0) continue;
            const long amount = 1 + (from_balance / 2);
            store.put_all({{from, from_balance - amount}, {to, to_balance + amount}});
        }
    });

    std::vector<std::thread> auditors;
    for (int t = 0; t < 4; ++t) {
        auditors.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto snap = store.snapshot();
                long total = 0;
                snap.for_each([&](int, long balance) { total += balance; });
                if (total != expected_total) broken.store(true);
                observations.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    stop.store(true);
    transfers.join();
    for (auto& thread : auditors) thread.join();

    CHECK(observations.load() > 100);
    CHECK(!broken.load());
}

TEST(gc_reclaims_only_unreachable_versions) {
    IntStore store(4);
    for (int i = 0; i < 20; ++i) store.put(0, i);
    CHECK(store.stats().longest_chain > 1);

    {
        auto pinned = store.snapshot();
        store.put(0, 999);
        store.gc();
        // pinned version is still reachable so its entry survives
        CHECK_EQ(pinned.get(0).value(), 19L);
        CHECK(store.stats().longest_chain >= 2);
    }

    store.gc();
    const auto stats = store.stats();
    CHECK_EQ(stats.longest_chain, 1u);
    CHECK_EQ(stats.live_snapshots, 0u);
    CHECK_EQ(store.get(0).value(), 999L);
}

TEST(gc_drops_keys_whose_tombstone_is_unreachable) {
    IntStore store(4);
    store.put(1, 1);
    store.put(2, 2);
    store.erase(1);
    CHECK_EQ(store.stats().keys, 2u);

    store.gc();
    const auto stats = store.stats();
    CHECK_EQ(stats.keys, 1u);       // the tombstoned key is gone entirely
    CHECK_EQ(stats.live_keys, 1u);
    CHECK(!store.get(1).has_value());
    CHECK_EQ(store.get(2).value(), 2L);
}

TEST(gc_runs_safely_alongside_traffic) {
    IntStore store(8);
    std::atomic<bool> stop{false};
    std::atomic<bool> corrupt{false};

    std::vector<std::thread> threads;
    for (int t = 0; t < 3; ++t) {
        threads.emplace_back([&] {
            long round = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                for (int i = 0; i < 100; ++i) store.put(i, round);
                ++round;
            }
        });
    }
    threads.emplace_back([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            auto snap = store.snapshot();
            snap.for_each([&](int, long value) {
                if (value < 0) corrupt.store(true);
            });
        }
    });
    threads.emplace_back([&] {
        while (!stop.load(std::memory_order_relaxed)) store.gc();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true);
    for (auto& thread : threads) thread.join();
    CHECK(!corrupt.load());
}

namespace {

// value whose copy stalls inside the store so a test can hold a write half
// installed and see what a concurrent snapshot gets, only the sentinal stalls
struct GatedValue {
    long payload = 0;
    static constexpr long kSentinel = 424242;
    static std::atomic<bool> gate_closed;
    static std::atomic<bool> writer_inside;

    GatedValue() = default;
    GatedValue(long value) : payload(value) {}  // NOLINT implicit on purpose
    GatedValue(GatedValue&&) = default;
    GatedValue& operator=(GatedValue&&) = default;
    GatedValue& operator=(const GatedValue&) = default;

    GatedValue(const GatedValue& other) : payload(other.payload) {
        if (payload != kSentinel) return;
        writer_inside.store(true, std::memory_order_release);
        while (gate_closed.load(std::memory_order_acquire)) std::this_thread::yield();
    }
};

std::atomic<bool> GatedValue::gate_closed{false};
std::atomic<bool> GatedValue::writer_inside{false};

} // namespace

TEST(watermark_hides_a_write_that_is_still_installing) {
    KVStore<int, GatedValue> store(8);
    store.put(1, GatedValue(1));

    GatedValue::gate_closed.store(true, std::memory_order_release);
    GatedValue::writer_inside.store(false, std::memory_order_release);

    std::atomic<uint64_t> write_version{0};
    std::thread writer([&] { write_version.store(store.put(2, GatedValue(GatedValue::kSentinel))); });

    while (!GatedValue::writer_inside.load(std::memory_order_acquire)) std::this_thread::yield();

    // write holds a version but isnt installed yet, a snapshot taken now has
    // to sit below it or it sees a gap in the history
    const uint64_t observed = store.version();
    auto snap = store.snapshot();

    GatedValue::gate_closed.store(false, std::memory_order_release);
    writer.join();

    CHECK(observed < write_version.load());
    CHECK(snap.version() < write_version.load());
    CHECK(!snap.get(2).has_value());          // invisible even after the write lands
    CHECK_EQ(snap.get(1).value().payload, 1L);
    CHECK_EQ(store.snapshot().get(2).value().payload, GatedValue::kSentinel);
}

TEST(watermark_is_monotonic_under_concurrent_writes) {
    IntStore store(8);
    std::atomic<bool> stop{false};
    std::atomic<bool> broken{false};

    std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t) {
        writers.emplace_back([&, t] {
            long round = 0;
            while (!stop.load(std::memory_order_relaxed)) store.put(t, round++);
        });
    }
    std::thread checker([&] {
        uint64_t previous = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            auto snap = store.snapshot();
            // snapshots never go backwards or claim a version the store hasnt reached
            if (snap.version() < previous) broken.store(true);
            if (snap.version() > store.version()) broken.store(true);
            previous = snap.version();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true);
    for (auto& thread : writers) thread.join();
    checker.join();
    CHECK(!broken.load());
}
