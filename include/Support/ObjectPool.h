#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"

namespace clice {

class StringSet {
public:
    using ID = std::uint32_t;

    StringSet(llvm::BumpPtrAllocator& allocator) : allocator(allocator) {
        strings.emplace_back();
    }

    StringSet(const StringSet&) = delete;

    StringSet(StringSet&&) = delete;

    StringSet& operator=(const StringSet&) = delete;

    StringSet& operator=(StringSet&&) = delete;

    ~StringSet() = default;

    ID get(llvm::StringRef s) {
        if(s.empty()) {
            return ID(0);
        }

        auto [it, success] = cache.try_emplace(s, ID(0));
        if(!success) {
            return it->second;
        }

        const auto size = s.size();
        auto p = allocator.Allocate<char>(size + 1);
        std::memcpy(p, s.data(), size);
        p[size] = '\0';

        it->first = llvm::StringRef(p, size);
        it->second = ID(strings.size());
        strings.emplace_back(it->first);
        return it->second;
    }

    llvm::StringRef get(ID id) {
        assert(id < strings.size());
        return strings[id];
    }

    llvm::StringRef save(llvm::StringRef s) {
        return get(get(s));
    }

private:
    llvm::BumpPtrAllocator& allocator;

    std::vector<llvm::StringRef> strings;

    llvm::DenseMap<llvm::StringRef, ID> cache;
};

template <typename T>
struct object_ptr {
    T* ptr = nullptr;

    object_ptr() noexcept = default;

    object_ptr(std::nullptr_t) noexcept : ptr(nullptr) {}

    explicit object_ptr(T* p) noexcept : ptr(p) {}

    T& operator*() const noexcept {
        return *ptr;
    }

    T* operator->() const noexcept {
        return ptr;
    }

    explicit operator bool() const noexcept {
        return ptr != nullptr;
    }

    std::strong_ordering operator<=>(const object_ptr&) const = default;
};

template <typename T>
object_ptr(T*) -> object_ptr<T>;

template <typename T>
class ObjectSet {
public:
    using ID = std::uint32_t;

    ObjectSet(llvm::BumpPtrAllocator& allocator) : allocator(allocator) {}

    ObjectSet(const ObjectSet&) = delete;

    ObjectSet(ObjectSet&&) = delete;

    ObjectSet& operator=(const ObjectSet&) = delete;

    ObjectSet& operator=(ObjectSet&&) = delete;

    ~ObjectSet() {
        if constexpr(!std::is_trivially_destructible_v<T>) {
            for(auto object: objects) {
                if(object) {
                    std::destroy_at(object.ptr);
                }
            }

            for(auto& [object, _]: removed) {
                if(object) {
                    std::destroy_at(object.ptr);
                }
            }
        }
    }

    ID get(const T& object) {
        auto [it, success] = cache.try_emplace(object_ptr(const_cast<T*>(&object)), ID(0));
        if(!success) {
            return it->second;
        }

        if(!removed.empty()) [[unlikely]] {
            auto [o, id] = removed.pop_back_val();

            /// Reuse the old memory
            std::destroy_at(o.ptr);
            new (o.ptr) T(object);

            it->first = o;
            it->second = id;

            /// Resume the object id.
            objects[id] = o;
        } else {
            /// Alloc the new memory.
            auto p = allocator.Allocate<T>(1);
            p = new (p) T(object);

            it->first = object_ptr<T>{p};
            it->second = ID(objects.size());

            objects.emplace_back(p);
        }

        return it->second;
    }

    object_ptr<T> get(ID id) {
        assert(id < objects.size());
        return objects[id];
    }

    object_ptr<T> save(const T& object) {
        return this->get(this->get(object));
    }

    void remove(object_ptr<T> object) {
        auto it = cache.find(object_ptr<T>(object.ptr));
        if(it == cache.end()) {
            return;
        }

        auto id = it->second;
        removed.emplace_back(object, id);
        cache.erase(it);
        objects[id] = nullptr;
    }

private:
    llvm::BumpPtrAllocator& allocator;

    std::vector<object_ptr<T>> objects;

    llvm::SmallVector<std::pair<object_ptr<T>, ID>> removed;

    llvm::DenseMap<object_ptr<T>, ID> cache;
};

}  // namespace clice

namespace llvm {

template <typename T>
struct DenseMapInfo<clice::object_ptr<T>> {
    using U = std::remove_cvref_t<T>;
    using O = clice::object_ptr<T>;

    inline static O getEmptyKey() {
        return O(DenseMapInfo<U*>::getEmptyKey());
    }

    inline static O getTombstoneKey() {
        return O(DenseMapInfo<U*>::getTombstoneKey());
    }

    inline static unsigned getHashValue(O value) {
        return DenseMapInfo<U>::getHashValue(*value);
    }

    inline static bool isEqual(O lhs, O rhs) {
        if(lhs == rhs) {
            return true;
        };

        const O Empty = getEmptyKey();
        const O Tombstone = getTombstoneKey();

        if(lhs == Empty || rhs == Empty || lhs == Tombstone || rhs == Tombstone) {
            return false;
        }

        return DenseMapInfo<U>::isEqual(*lhs, *rhs);
    }
};

}  // namespace llvm
