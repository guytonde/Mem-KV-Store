#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "core.hpp"

namespace kvstore {

template <typename Key, typename Value, typename Hash, typename KeyEqual>
class KVStore;

// Bassically an immutable view of a KVStore. Creating one is O(shards) and
// independent of how much data is stored: it pins a version instead of
// copying, so it never blocks writers. Every read goes through that version,
// so the view is stable for the snapshot's whole life. Keeps the store's state
// alive and may outlive the KVStore itself.
//
// Only KVStore::snapshot() can build one. The version it pins has to be a
// watermark that was read and pinned under the registry lock, or the view
// could sit above the install frontier or below the gc bound.
template <typename Key, typename Value, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
class Snapshot {
public:
    // had a stroke trying to remember what each was; simplifying here
    using key_type = Key;
    using value_type = Value;
    using size_type = std::size_t;
    using core_type = detail::Core<Key, Value, Hash, KeyEqual>;

    ~Snapshot() {
        if (core_) core_->snapshots.release(version_);
    }

    Snapshot(const Snapshot& other) : core_(other.core_), version_(other.version_) {
        if (core_) core_->snapshots.acquire(version_);
    }

    Snapshot(Snapshot&& other) noexcept
        : core_(std::move(other.core_)), version_(other.version_) {
        other.core_ = nullptr;
    }

    Snapshot& operator=(const Snapshot& other) {
        if (this != &other) {
            Snapshot copy(other);
            swap(copy);
        }
        return *this;
    }

    Snapshot& operator=(Snapshot&& other) noexcept {
        if (this != &other) {
            Snapshot moved(std::move(other));
            swap(moved);
        }
        return *this;
    }

    void swap(Snapshot& other) noexcept {
        core_.swap(other.core_);
        std::swap(version_, other.version_);
    }

    std::optional<value_type> get(const key_type& key) const {
        auto& shard = core_->shards[core_->shard_of(key)];
        util::SharedLock lock(shard.mutex);
        auto it = shard.map.find(key);
        if (it == shard.map.end()) return std::nullopt;
        const auto* entry = core_type::find_at(it->second, version_);
        if (entry == nullptr || entry->tombstone) return std::nullopt;
        return entry->value;
    }

    bool contains(const key_type& key) const { return get(key).has_value(); }

    // live keyes at this version
    size_type size() const {
        size_type count = 0;
        for_each([&count](const key_type&, const value_type&) { ++count; });
        return count;
    }

    bool empty() const { return size() == 0; }

    // fn(key, value) over every key live at this version. One shard at a time
    // under a read lock, so writers to other shards keep running. The locks are
    // not reentrant: fn must not call back into this snapshot or the store, or
    // it can deadlock on the shard it is being called from.
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (std::size_t i = 0; i < core_->shard_count; ++i) {
            auto& shard = core_->shards[i];
            util::SharedLock lock(shard.mutex);
            for (const auto& [key, chain] : shard.map) {
                const auto* entry = core_type::find_at(chain, version_);
                if (entry != nullptr && !entry->tombstone) fn(key, entry->value);
            }
        }
    }

    // materialize the view (for tests)
    std::vector<std::pair<key_type, value_type>> items() const {
        std::vector<std::pair<key_type, value_type>> out;
        for_each([&out](const key_type& key, const value_type& value) {
            out.emplace_back(key, value);
        });
        return out;
    }

    uint64_t version() const noexcept { return version_; }

private:
    friend class KVStore<Key, Value, Hash, KeyEqual>;

    // Takes over a pin the store already registered for version
    Snapshot(std::shared_ptr<core_type> core, uint64_t version)
        : core_(std::move(core)), version_(version) {}

    std::shared_ptr<core_type> core_;
    uint64_t version_;
};

} // namespace kvstore
