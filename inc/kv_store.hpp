#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "core.hpp"
#include "snapshot.hpp"

namespace kvstore {

//  counters for debug
struct Stats {
    std::size_t shards = 0;
    std::size_t keys = 0;            // keys with at least one entry + tombstones
    std::size_t live_keys = 0;       // keys whose newest entry is real
    std::size_t versions = 0;        // total entries retained across all chains
    std::size_t longest_chain = 0;
    std::size_t live_snapshots = 0;
    uint64_t version = 0;            // current commit watermark
};

// The actual thread-safe in-mem key-value store with consistent snapshots
template <typename Key, typename Value, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
class KVStore {
public:
    using key_type = Key;
    using value_type = Value;
    using size_type = std::size_t;
    using snapshot_type = Snapshot<Key, Value, Hash, KeyEqual>;
    using core_type = detail::Core<Key, Value, Hash, KeyEqual>;

    // shard_count is the number of independent lock stripes, minimum 1
    explicit KVStore(std::size_t shard_count = default_shard_count())
        : core_(std::make_shared<core_type>(std::max<std::size_t>(1, shard_count))) {}

    ~KVStore() = default;
    KVStore(const KVStore&) = delete;
    KVStore& operator=(const KVStore&) = delete;

    // Newest committed value or nullopt if absent or deleted
    std::optional<value_type> get(const key_type& key) const {
        auto& shard = core_->shards[core_->shard_of(key)];
        util::SharedLock lock(shard.mutex);
        auto it = shard.map.find(key);
        if (it == shard.map.end() || it->second.empty()) return std::nullopt;
        const auto& newest = it->second.back();
        if (newest.tombstone) return std::nullopt;
        return newest.value;
    }

    bool contains(const key_type& key) const { return get(key).has_value(); }

    uint64_t put(const key_type& key, const value_type& value) {
        const std::size_t index = core_->shard_of(key);
        auto& shard = core_->shards[index];
        util::ExclusiveLock lock(shard.mutex);
        const uint64_t version = core_->begin_write(index);
        core_type::install(shard.map[key], {version, false, value});
        core_->end_write(index);
        return version;
    }

    // Apply several writes under one version
    uint64_t put_all(const std::vector<std::pair<key_type, value_type>>& entries) {
        MultiShardLock lock(*core_, shards_for(entries));
        const uint64_t version = core_->begin_write(lock.indices().data(), lock.indices().size());
        for (const auto& [key, value] : entries) {
            core_type::install(core_->shards[core_->shard_of(key)].map[key],
                               {version, false, value});
        }
        core_->end_write(lock.indices().data(), lock.indices().size());
        return version;
    }

    uint64_t put_all(std::initializer_list<std::pair<key_type, value_type>> entries) {
        return put_all(std::vector<std::pair<key_type, value_type>>(entries));
    }

    //replace the value only if it currently equals `expected`
    bool compare_and_put(const key_type& key, const value_type& expected,
                         const value_type& desired) {
        const std::size_t index = core_->shard_of(key);
        auto& shard = core_->shards[index];
        util::ExclusiveLock lock(shard.mutex);
        auto it = shard.map.find(key);
        if (it == shard.map.end() || it->second.empty() || it->second.back().tombstone ||
            !(it->second.back().value == expected)) {
            return false;
        }
        const uint64_t version = core_->begin_write(index);
        core_type::install(it->second, {version, false, desired});
        core_->end_write(index);
        return true;
    }

    // Delete by appending a tombstone so older snapshots keep their value
    // False if the key was already gone.
    bool erase(const key_type& key) {
        const std::size_t index = core_->shard_of(key);
        auto& shard = core_->shards[index];
        util::ExclusiveLock lock(shard.mutex);
        auto it = shard.map.find(key);
        if (it == shard.map.end() || it->second.empty() || it->second.back().tombstone) {
            return false;
        }
        const uint64_t version = core_->begin_write(index);
        core_type::install(it->second, {version, true, value_type{}});
        core_->end_write(index);
        return true;
    }

    // Consistent view as of now, O(shards), independent of data size, pins a
    // version instead of copying
    snapshot_type snapshot() const {
        const uint64_t version =
            core_->snapshots.pin_current([this] { return core_->watermark(); });
        return snapshot_type(core_, version);
    }

    size_type size() const {
        size_type count = 0;
        for (std::size_t i = 0; i < core_->shard_count; ++i) {
            auto& shard = core_->shards[i];
            util::SharedLock lock(shard.mutex);
            for (const auto& [key, chain] : shard.map) {
                (void)key;
                if (!chain.empty() && !chain.back().tombstone) ++count;
            }
        }
        return count;
    }

    bool empty() const { return size() == 0; }

    // Tombstone every live key under one version
    void clear() {
        std::vector<std::size_t> all(core_->shard_count);
        for (std::size_t i = 0; i < core_->shard_count; ++i) all[i] = i;
        // All shards held at once, so nobody can catch a half-cleared store.
        MultiShardLock lock(*core_, std::move(all));
        const uint64_t version = core_->begin_write(lock.indices().data(), lock.indices().size());
        for (std::size_t i = 0; i < core_->shard_count; ++i) {
            for (auto& [key, chain] : core_->shards[i].map) {
                (void)key;
                if (!chain.empty() && !chain.back().tombstone) {
                    core_type::install(chain, {version, true, value_type{}});
                }
            }
        }
        core_->end_write(lock.indices().data(), lock.indices().size());
    }

    // drop versions no live snapshot can see, and forget keys left with only a
    // reclaimable tombstone
    size_type gc() {
        const uint64_t bound = core_->snapshots.gc_bound([this] { return core_->watermark(); });
        size_type reclaimed = 0;
        for (std::size_t i = 0; i < core_->shard_count; ++i) {
            auto& shard = core_->shards[i];
            util::ExclusiveLock lock(shard.mutex);
            for (auto it = shard.map.begin(); it != shard.map.end();) {
                auto& chain = it->second;
                // everything below the newest entry visible at bound is unreachable
                std::size_t keep_from = 0;
                for (std::size_t j = 0; j < chain.size(); ++j) {
                    if (chain[j].version <= bound) keep_from = j;
                    else break;
                }
                if (keep_from > 0) {
                    chain.erase(chain.begin(), chain.begin() + static_cast<long>(keep_from));
                    reclaimed += keep_from;
                }
                if (chain.size() == 1 && chain.front().tombstone && chain.front().version <= bound) {
                    it = shard.map.erase(it);
                    ++reclaimed;
                } else {
                    ++it;
                }
            }
        }
        return reclaimed;
    }

    // commit watermark, everything at or below it is fully installed.
    uint64_t version() const { return core_->watermark(); }

    size_type shard_count() const { return core_->shard_count; }

    Stats stats() const {
        Stats out;
        out.shards = core_->shard_count;
        out.version = core_->watermark();
        out.live_snapshots = core_->snapshots.live_snapshots();
        for (std::size_t i = 0; i < core_->shard_count; ++i) {
            auto& shard = core_->shards[i];
            util::SharedLock lock(shard.mutex);
            for (const auto& [key, chain] : shard.map) {
                (void)key;
                ++out.keys;
                out.versions += chain.size();
                out.longest_chain = std::max(out.longest_chain, chain.size());
                if (!chain.empty() && !chain.back().tombstone) ++out.live_keys;
            }
        }
        return out;
    }

    static std::size_t default_shard_count() {
        const unsigned hw = std::thread::hardware_concurrency();
        return std::max(4u, hw == 0 ? 8u : hw * 2);
    }

private:
    // lock shards in ascending index order, release them in reverse
    class MultiShardLock {
    public:
        MultiShardLock(core_type& core, std::vector<std::size_t> shards)
            : core_(core), shards_(std::move(shards)) {
            for (std::size_t index : shards_) core_.shards[index].mutex.lock();
        }

        ~MultiShardLock() {
            for (auto it = shards_.rbegin(); it != shards_.rend(); ++it) {
                core_.shards[*it].mutex.unlock();
            }
        }

        MultiShardLock(const MultiShardLock&) = delete;
        MultiShardLock& operator=(const MultiShardLock&) = delete;

        const std::vector<std::size_t>& indices() const { return shards_; }

    private:
        core_type& core_;
        std::vector<std::size_t> shards_;
    };

    std::vector<std::size_t> shards_for(
        const std::vector<std::pair<key_type, value_type>>& entries) const {
        std::vector<std::size_t> indices;
        indices.reserve(entries.size());
        for (const auto& [key, value] : entries) {
            (void)value;
            indices.push_back(core_->shard_of(key));
        }
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        return indices;
    }

    std::shared_ptr<core_type> core_;
};

} // namespace kvstore
