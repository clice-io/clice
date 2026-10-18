namespace std {
namespace __exception_ptr {
class exception_ptr {};
}  // namespace __exception_ptr

using __exception_ptr::exception_ptr;

template <class Pointer>
struct __wrap_iter {};

template <class T>
struct vector {
    using iterator = __wrap_iter<T*>;
    T& front();
    iterator begin();
};
}  // namespace std

struct Root {
    using iterator = float;
    iterator find();
};

struct Middle : Root {
    using iterator = int;
};

struct Leaf : Middle {};

template <class Key, class Value>
struct MapIterator {};

template <class Derived, class Key, class Value>
struct MapBase {
    using iterator = MapIterator<Key, Value>;
    iterator find(Key key);
};

struct Registry : MapBase<Registry, int, long> {
    void scan() {
        auto it = find(1);
    }
};

class Stack : private std::vector<int> {
public:
    using vector::begin;
};

void use(std::vector<std::exception_ptr>& errors, Leaf& leaf, Stack& stack) {
    auto first = errors.front();
    auto found = leaf.find();
    auto top = stack.begin();
}
