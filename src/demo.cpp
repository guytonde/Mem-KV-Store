// walkthrough of snapshot isolation, atomic batches, concurent traffic and gc
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "kv_store.hpp"

using Clock = std::chrono::steady_clock;

namespace {

void heading(const char* text) { std::printf("\n== %s ==\n", text); }

void print_stats(const kvstore::Stats& stats) {
    std::printf("  shards=%zu keys=%zu live=%zu versions=%zu longest_chain=%zu "
                "snapshots=%zu version=%llu\n",
                stats.shards, stats.keys, stats.live_keys, stats.versions, stats.longest_chain,
                stats.live_snapshots, static_cast<unsigned long long>(stats.version));
}

} // namespace

int main() {
    heading("snapshot isolation");
    {
        kvstore::KVStore<std::string, std::string> store;
        store.put("user:1", "alice");
        store.put("user:2", "bob");

        auto snap = store.snapshot();
        store.put("user:1", "alice-renamed");
        store.erase("user:2");
        store.put("user:3", "carol");

        std::printf("  snapshot @v%llu : user:1=%s user:2=%s user:3=%s (size=%zu)\n",
                    static_cast<unsigned long long>(snap.version()),
                    snap.get("user:1").value_or("<none>").c_str(),
                    snap.get("user:2").value_or("<none>").c_str(),
                    snap.get("user:3").value_or("<none>").c_str(), snap.size());
        std::printf("  live     @v%llu : user:1=%s user:2=%s user:3=%s (size=%zu)\n",
                    static_cast<unsigned long long>(store.version()),
                    store.get("user:1").value_or("<none>").c_str(),
                    store.get("user:2").value_or("<none>").c_str(),
                    store.get("user:3").value_or("<none>").c_str(), store.size());
    }

    heading("atomic multi-key writes under concurrent audits");
    {
        kvstore::KVStore<int, long> ledger(8);
        constexpr int kAccounts = 8;
        constexpr long kStart = 1000;
        for (int i = 0; i < kAccounts; ++i) ledger.put(i, kStart);

        std::atomic<bool> stop{false};
        std::atomic<long> audits{0};
        std::atomic<long> transfers{0};
        std::atomic<bool> torn{false};

        std::thread mover([&] {
            unsigned seed = 7;
            while (!stop.load(std::memory_order_relaxed)) {
                seed = seed * 1103515245u + 12345u;
                const int from = static_cast<int>((seed >> 16) % kAccounts);
                const int to = (from + 1) % kAccounts;
                const long balance = ledger.get(from).value();
                if (balance <= 0) continue;
                const long amount = 1 + balance / 2;
                ledger.put_all({{from, balance - amount}, {to, ledger.get(to).value() + amount}});
                transfers.fetch_add(1, std::memory_order_relaxed);
            }
        });

        std::vector<std::thread> auditors;
        for (int t = 0; t < 3; ++t) {
            auditors.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    auto snap = ledger.snapshot();
                    long total = 0;
                    snap.for_each([&](int, long balance) { total += balance; });
                    if (total != kAccounts * kStart) torn.store(true);
                    audits.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stop.store(true);
        mover.join();
        for (auto& thread : auditors) thread.join();

        std::printf("  %ld transfers, %ld audits, torn reads: %s\n", transfers.load(),
                    audits.load(), torn.load() ? "YES (bug!)" : "none");
    }

    heading("snapshot cost is independent of store size");
    {
        kvstore::KVStore<long, long> store;
        for (long i = 0; i < 200000; ++i) store.put(i, i);
        store.gc();

        const auto start = Clock::now();
        for (int i = 0; i < 10000; ++i) {
            auto snap = store.snapshot();
            (void)snap;
        }
        const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        std::printf("  200k keys, 10k snapshots: %.0f ns per snapshot\n", elapsed / 10000);
    }

    heading("version reclamation");
    {
        kvstore::KVStore<int, long> store(4);
        for (int i = 0; i < 1000; ++i) store.put(i % 10, i);
        std::printf("  after 1000 writes to 10 keys:\n");
        print_stats(store.stats());

        {
            auto pinned = store.snapshot();
            const long pinned_value = pinned.get(0).value();
            for (int i = 1000; i < 2000; ++i) store.put(i % 10, i);

            std::printf("  gc() while a snapshot is pinned reclaimed %zu entries:\n", store.gc());
            print_stats(store.stats());
            std::printf("  the pinned version survives: snapshot key 0 = %ld, live key 0 = %ld\n",
                        pinned.get(0).value(), store.get(0).value());
            if (pinned.get(0).value() != pinned_value) std::printf("  (bug: snapshot moved!)\n");
        }

        std::printf("  gc() once it is released reclaimed %zu more entries:\n", store.gc());
        print_stats(store.stats());
    }

    std::printf("\n");
    return 0;
}
