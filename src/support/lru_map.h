#pragma once

#include <cassert>
#include <cstddef>
#include <functional>
#include <list>
#include <utility>

#include "llvm/ADT/DenseMap.h"

namespace clice {

/// A key-value map with LRU eviction policy.
///
/// - `find()` / `operator[]` / `touch()` promote the entry to most-recently-used.
/// - When inserting beyond capacity, the least-recently-used entry is evicted.
/// - An optional `on_evict` callback is invoked just before eviction.
/// - Supports range-based iteration (most-recent first).
///
/// Key must be usable with llvm::DenseMap (i.e. has DenseMapInfo specialization).
template <typename Key, typename Value>
class LRUMap {
    using Entry = std::pair<Key, Value>;
    using List = std::list<Entry>;
    using Iterator = typename List::iterator;

public:
    explicit LRUMap(std::size_t capacity = 0) : capacity_(capacity) {}

    LRUMap(const LRUMap&) = delete;
    LRUMap& operator=(const LRUMap&) = delete;
    LRUMap(LRUMap&&) = default;
    LRUMap& operator=(LRUMap&&) = default;

    /// Set the maximum number of entries. If the new capacity is smaller,
    /// excess entries are evicted immediately (LRU order).
    void set_capacity(std::size_t cap) {
        capacity_ = cap;
        shrink();
    }

    std::size_t capacity() const {
        return capacity_;
    }

    std::size_t size() const {
        return index_.size();
    }

    bool empty() const {
        return index_.empty();
    }

    /// Lookup an entry. Returns nullptr if not found. Promotes to MRU.
    Value* find(const Key& key) {
        auto it = index_.find(key);
        if(it == index_.end())
            return nullptr;
        promote(it->second);
        return &it->second->second;
    }

    /// Lookup an entry without promoting it (no side effects).
    const Value* peek(const Key& key) const {
        auto it = index_.find(key);
        if(it == index_.end())
            return nullptr;
        return &it->second->second;
    }

    /// Check if a key exists (does NOT promote).
    bool contains(const Key& key) const {
        return index_.count(key);
    }

    /// Insert or access. If the key exists, promotes and returns existing value.
    /// If not, default-constructs the value, inserts as MRU, and may evict LRU.
    Value& operator[](const Key& key) {
        auto it = index_.find(key);
        if(it != index_.end()) {
            promote(it->second);
            return it->second->second;
        }
        return insert_new(key, Value{});
    }

    /// Insert a key-value pair. If the key already exists, the value is replaced
    /// and the entry is promoted. Returns a reference to the stored value.
    Value& insert(const Key& key, Value value) {
        auto it = index_.find(key);
        if(it != index_.end()) {
            it->second->second = std::move(value);
            promote(it->second);
            return it->second->second;
        }
        return insert_new(key, std::move(value));
    }

    /// Insert if not present. Returns {reference, true} on insertion,
    /// {reference, false} if already present (promoted either way).
    template <typename... Args>
    std::pair<Value&, bool> try_emplace(const Key& key, Args&&... args) {
        auto it = index_.find(key);
        if(it != index_.end()) {
            promote(it->second);
            return {it->second->second, false};
        }
        auto& ref = insert_new(key, Value(std::forward<Args>(args)...));
        return {ref, true};
    }

    /// Remove an entry explicitly (no eviction callback).
    bool erase(const Key& key) {
        auto it = index_.find(key);
        if(it == index_.end())
            return false;
        order_.erase(it->second);
        index_.erase(it);
        return true;
    }

    /// Promote an entry to MRU without returning it.
    void touch(const Key& key) {
        auto it = index_.find(key);
        if(it != index_.end())
            promote(it->second);
    }

    void clear() {
        order_.clear();
        index_.clear();
    }

    /// Callback invoked just before an entry is evicted due to capacity overflow.
    /// Signature: void(Key, Value&). Not called for explicit erase().
    std::function<void(Key, Value&)> on_evict;

    // Range iteration (most-recent first).
    auto begin() {
        return order_.begin();
    }

    auto end() {
        return order_.end();
    }

    auto begin() const {
        return order_.begin();
    }

    auto end() const {
        return order_.end();
    }

private:
    void promote(Iterator it) {
        order_.splice(order_.begin(), order_, it);
    }

    Value& insert_new(const Key& key, Value&& value) {
        order_.emplace_front(key, std::move(value));
        index_[key] = order_.begin();
        shrink();
        return order_.front().second;
    }

    void shrink() {
        while(capacity_ > 0 && index_.size() > capacity_) {
            auto& victim = order_.back();
            if(on_evict)
                on_evict(victim.first, victim.second);
            index_.erase(victim.first);
            order_.pop_back();
        }
    }

    std::size_t capacity_;
    List order_;
    llvm::DenseMap<Key, Iterator> index_;
};

}  // namespace clice
