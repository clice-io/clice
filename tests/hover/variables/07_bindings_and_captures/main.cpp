// Names introduced without a type of their own: structured bindings, of a
// tuple-like class too, a range-for variable, a lambda capture, init
// captures.

namespace std {
template <typename T> struct tuple_size;
template <unsigned long Index, typename T> struct tuple_element;
}  // namespace std

struct Pair {
    int first;
    double second;
};

Pair make();

struct Entry {
    template <unsigned long Index> auto get() const {
        if constexpr(Index == 0) {
            return key;
        } else {
            return weight;
        }
    }
    long key;
    float weight;
};

template <> struct std::tuple_size<Entry> {
    static constexpr unsigned long value = 2;
};
template <> struct std::tuple_element<0, Entry> {
    using type = long;
};
template <> struct std::tuple_element<1, Entry> {
    using type = float;
};

void bindings() {
    auto [§(binding)first, second] = make();
    (void)§(binding_use)first;
    int items[3]{};
    for (auto& §(range_variable)item : items) {
        (void)item;
    }
    static thread_local auto [§(thread_local_binding)shared, unused] = make();
    (void)shared;
    Pair pairs[2]{};
    for (auto [§(range_binding)key, value] : pairs) {
        (void)key;
    }
    auto [§(tuple_like_binding)key, weight] = Entry{};
    int threshold = 3;
    auto above = [&§(capture)threshold](int v) { return v > §(capture_use)threshold; };
    (void)above;
    auto scaled = [§(init_capture)limit = threshold * 2, &§(reference_init_capture)bound = threshold] {
        return limit + bound;
    };
    (void)scaled;
}
