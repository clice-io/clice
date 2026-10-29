module clice:support.object_pool;

import :support.prelude;

namespace clice {

template <typename T>
struct object_ptr {
    T* ptr = nullptr;

    object_ptr() noexcept = default;

    explicit object_ptr(T* p) noexcept : ptr(p) {}

    T& operator*() const noexcept {
        return *ptr;
    }

    T* operator->() const noexcept {
        return ptr;
    }

    std::strong_ordering operator<=>(const object_ptr&) const = default;
};

template <typename T>
object_ptr(T*) -> object_ptr<T>;

/// Values of T interned in a bump allocator, each named by a dense ID. The
/// allocator never runs destructors, hence the trivially destructible T.
template <typename T>
class ObjectSet {
    static_assert(std::is_trivially_destructible_v<T>);

public:
    using ID = std::uint32_t;

    explicit ObjectSet(llvm::BumpPtrAllocator* allocator) : allocator(allocator) {}

    ObjectSet(const ObjectSet&) = delete;
    ObjectSet& operator=(const ObjectSet&) = delete;

    ID get(const T& object) {
        auto [it, success] = cache.try_emplace(object_ptr(const_cast<T*>(&object)), ID(0));
        if(!success) {
            return it->second;
        }

        auto* p = new (allocator->Allocate<T>(1)) T(object);
        it->first = object_ptr<T>{p};
        it->second = ID(objects.size());
        objects.emplace_back(p);
        return it->second;
    }

    object_ptr<T> get(ID id) const {
        assert(id < objects.size());
        return objects[id];
    }

private:
    llvm::BumpPtrAllocator* allocator;
    std::vector<object_ptr<T>> objects;
    llvm::DenseMap<object_ptr<T>, ID> cache;
};

}  // namespace clice

namespace llvm {

template <typename T>
struct DenseMapInfo<clice::object_ptr<T>> {
    using U = std::remove_cvref_t<T>;
    using O = clice::object_ptr<T>;

    inline static unsigned getHashValue(O value) {
        return DenseMapInfo<U>::getHashValue(*value);
    }

    inline static bool isEqual(O lhs, O rhs) {
        return lhs == rhs || DenseMapInfo<U>::isEqual(*lhs, *rhs);
    }
};

}  // namespace llvm
