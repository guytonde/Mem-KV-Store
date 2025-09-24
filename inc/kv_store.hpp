#pragma once
#include <memory>
#include <optional>
#include <atomic>
#include "snapshot.hpp"
#include "rwmutex.hpp"
using namespace std;

namespace kvstore {

/**
 * Yet another thread-safe key-value store with immutable snapshots.
 * 
 * @tparam Key            Key type
 * @tparam Value          Mapped type
 * @tparam Container      Underlying container (must have map‐like API)
 * @tparam Hash           Hash functor for Key (if unordered)
 * @tparam KeyEqual       Key equality functor (if unordered)
 * 
 * 
 *  */ 
template<
    typename Key,
    typename Value,
    typename Container = unordered_map<Key, Value>,
    typename Hash = hash<Key>,
    typename KeyEqual = equal_to<Key>
>
class KVStore {
public:
    using key_type = Key;
    using value_type = Value;
    using container_type = Container;
    using size_type = typename container_type::size_type;
    using snapshot_ptr = shared_ptr<Snapshot<Key,Value,Container,Hash,KeyEqual>>;

    KVStore() = default;
    ~KVStore() = default;
    KVStore(const KVStore&) = delete;
    KVStore& operator=(const KVStore&) = delete;

    optional<value_type> get(const key_type& key) const {
        util::SharedLock lock(mutex_);
        auto it = data_.find(key);
        if (it != data_.end()) return it->second;
        return nullopt;
    }

    void put(const key_type& key, const value_type& value) {
        util::ExclusiveLock lock(mutex_);
        data_[key] = value;
        version_.fetch_add(1, memory_order_relaxed);
    }

    bool erase(const key_type& key) {
        util::ExclusiveLock lock(mutex_);
        auto it = data_.find(key);
        if (it == data_.end()) return false;
        data_.erase(it);
        version_.fetch_add(1, memory_order_relaxed);
        return true;
    }

    // Create an immutable snapshot
    snapshot_ptr snapshot() const {
        util::SharedLock lock(mutex_);
        uint64_t ver = version_.load(memory_order_acquire);
        return make_shared<Snapshot<Key,Value,Container,Hash,KeyEqual>>(data_, ver);
    }

    size_type size() const {
        util::SharedLock lock(mutex_);
        return data_.size();
    }

    bool empty() const {
        util::SharedLock lock(mutex_);
        return data_.empty();
    }

private:
    mutable Container data_;
    mutable util::RWMutex mutex_;
    mutable atomic<uint64_t> version_{0};
};

} // namespace kvstore
