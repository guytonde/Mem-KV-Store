#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "mutex.hpp"
#include "rwmutex.hpp"

namespace kvstore {
namespace detail {

inline constexpr uint64_t kNoWrite = std::numeric_limits<uint64_t>::max();

// A write has claimed the shard but has not drawn its version yet. Versions
// start at 1 so 0 is free!! A watermark scan that lands on this spins the few
// instructions until the real version shows up.
inline constexpr uint64_t kReserving = 0;

// Refcounts the versions live snapshots are pinned to so gc knows whats
// still reachable
class SnapshotRegistry {
public:
    // Pin the current version and return it. Reading the version and
    // registering the pin under one lock is what stops gc from reclaiming in
    // between the two steps.
    template <typename ReadVersion>
    uint64_t pin_current(ReadVersion&& read_version) {
        util::LockGuard guard(mutex_);
        const uint64_t version = read_version();
        ++pins_[version];
        return version;
    }

    void acquire(uint64_t version) {
        util::LockGuard guard(mutex_);
        ++pins_[version];
    }

    void release(uint64_t version) {
        util::LockGuard guard(mutex_);
        auto it = pins_.find(version);
        if (it == pins_.end()) return;
        if (--it->second == 0) pins_.erase(it);
    }

    // Oldest version gc has to keep, the oldest pin or the current version if
    // nothing is pinned. Same lock as pin_current so a snapshot taken
    // concurrently is either counted here or pinned at or above whats returned
    template <typename ReadVersion>
    uint64_t gc_bound(ReadVersion&& read_version) const {
        util::LockGuard guard(mutex_);
        if (!pins_.empty()) return pins_.begin()->first;
        return read_version();
    }

    std::size_t live_snapshots() const {
        util::LockGuard guard(mutex_);
        std::size_t total = 0;
        for (const auto& [version, count] : pins_) {
            (void)version;
            total += count;
        }
        return total;
    }

private:
    mutable util::Mutex mutex_;
    std::map<uint64_t, std::size_t> pins_;
};

// Shared state behind a KVStore and every Snapshot taken from it.
//
// Keys are striped across shards, each with its own reader/writer lock, so
// unrelated keys never contend. Each key owns a version chain sorted ascending;
// a read at version V walks back to the newest entry with version <= V. Deletes
// append a tombstone, so a snapshot from before the delete still sees the value.
//
// BUG I FOUND :( --> snapshot must never see version 7 while 6 is still installing,
// or it sees a gap in the write history. Funnelling every commit through one
// shared watermark serializes the write path and made throughput fall as
// threads were added, so instead each shard publishes the version it is
// installing and a snapshot scans those fields. O(shards), independent of how
// much data is stored.
template <typename Key, typename Value, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
struct Core {
    struct Entry {
        uint64_t version;
        bool tombstone;
        Value value;
    };

    using Chain = std::vector<Entry>;
    using Map = std::unordered_map<Key, Chain, Hash, KeyEqual>;

    // Own cache line writers hit inflight on every write
    struct alignas(64) Shard {
        mutable util::RWMutex mutex;
        std::atomic<uint64_t> inflight{kNoWrite};
        Map map;
    };

    explicit Core(std::size_t shard_count)
        : shards(new Shard[shard_count]), shard_count(shard_count) {}

    std::unique_ptr<Shard[]> shards;
    std::size_t shard_count;
    std::atomic<uint64_t> clock{0};
    SnapshotRegistry snapshots;
    Hash hasher;

    std::size_t shard_of(const Key& key) const { return hasher(key) % shard_count; }

    // Reserve a version for a write into 'indices'; caller already holds those
    // shards exclusively. Shards are claimed before the version is drawn, so a
    // scan can't slip between the two steps and decide nothing is pending. The
    // exact version gets published, not a lower bound.
    uint64_t begin_write(const std::size_t* indices, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            shards[indices[i]].inflight.store(kReserving, std::memory_order_seq_cst);
        }
        const uint64_t version = clock.fetch_add(1, std::memory_order_seq_cst) + 1;
        // Versions must never collide with the two sentinels. 2^64 writes is
        // out of reach in practice, so this is a cheap guard, not a plan.
        if (version == kNoWrite) std::abort();
        for (std::size_t i = 0; i < count; ++i) {
            shards[indices[i]].inflight.store(version, std::memory_order_seq_cst);
        }
        return version;
    }

    uint64_t begin_write(std::size_t index) { return begin_write(&index, 1); }

    void end_write(const std::size_t* indices, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            shards[indices[i]].inflight.store(kNoWrite, std::memory_order_seq_cst);
        }
    }

    void end_write(std::size_t index) { end_write(&index, 1); }

    // Highest version with everything at or below it installed. Monotonic, and
    // O(shards) no matter how much data is stored. 
    uint64_t watermark() const {
        uint64_t safe = clock.load(std::memory_order_seq_cst);
        for (std::size_t i = 0; i < shard_count; ++i) {
            uint64_t pending = shards[i].inflight.load(std::memory_order_seq_cst);
            while (pending == kReserving) {  // version is being drawn rn
                util::cpu_relax();
                pending = shards[i].inflight.load(std::memory_order_seq_cst);
            }
            if (pending != kNoWrite && pending - 1 < safe) safe = pending - 1;
        }
        return safe;
    }

    // Newest entry <= version or nullptr if the key didn't exist yet
    static const Entry* find_at(const Chain& chain, uint64_t version) {
        // Chains stay short under gc, so scanning back beats a binary search.
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            if (it->version <= version) return &*it;
        }
        return nullptr;
    }

    // Insert keeping the chain sorted; concurrent writers install ooo
    static void install(Chain& chain, Entry entry) {
        if (chain.empty() || chain.back().version < entry.version) {
            chain.push_back(std::move(entry));
            return;
        }
        auto pos = std::upper_bound(
            chain.begin(), chain.end(), entry.version,
            [](uint64_t version, const Entry& e) { return version < e.version; });
        chain.insert(pos, std::move(entry));
    }
};

} // namespace detail
} // namespace kvstore
