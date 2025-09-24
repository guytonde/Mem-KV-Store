#pragma once
#include <unordered_map>
#include <optional>

using namespace std;

namespace kvstore {

template<
    typename Key,
    typename Value,
    typename Container = unordered_map<Key,Value>,
    typename Hash = hash<Key>,
    typename KeyEqual = equal_to<Key>
>
class Snapshot {
public:
    using key_type = Key;
    using value_type = Value;
    using container_type = Container;
    using size_type = typename container_type::size_type;

    Snapshot(const Container& data, uint64_t version) : data_(data), version_(version) {
// nothing for now

    }

    optional<value_type> get(const key_type& key) const {
        auto it = data_.find(key);
        if (it != data_.end()) return it->second;

        return nullopt;
    }

    bool contains(const key_type& key) const {
        return data_.find(key) != data_.end();
    }

    // getters

    uint64_t version() const noexcept { 
        return version_;
    }

    size_type size() const noexcept { 
        return data_.size(); 
    }

    bool empty() const noexcept { 
        return data_.empty(); 
    }

    auto begin() const noexcept { 
        return data_.begin(); 
    }
    auto end() const noexcept { 
        return data_.end();   
    }

private:
    const Container data_;
    const uint64_t version_;
};

} // namespace kvstore
