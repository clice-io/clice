// A use of an implicit instantiation shows the template's member summary
// and the arguments; a member class or a static member of the instantiation
// shows as the instantiation has it.

namespace instantiation_members {
struct Empty {};

struct Base {
    int base_value;
};

template <typename T, int Size> struct alignas(64) Cache final : Base {
    struct Node;
    enum Kind : int;
    enum class State { Idle, Busy };
    using value_type = T;
    [[no_unique_address]] Empty tag;
    alignas(32) T slots[Size];
    value_type* cursor;
    State state = State::Idle;
};

§(instantiation)Cache<long, 4> cache;

template <typename T> struct Hidden {
    T __first;
    T __second;
};

§(reserved_members)Hidden<int> hidden;

namespace inner {
template <typename T> struct Wrapper {
    struct Item {
        T value;
    };
    Item item;
    Item* next;
};
}

inner::§(nested_types)Wrapper<char> wrapper;
inner::Wrapper<char>::§(member_class)Item item;

template <typename T, T Value> struct Constant {
    static constexpr T value = Value;
    using size_type = unsigned long;
    size_type count = 0;
};

§(parameter_value)Constant<int, 7> seven;
int copied = Constant<int, 7>::§(static_member)value;

template <int Count> struct Slots {};

template <typename... Ts> struct Pack {
    using Table = Slots<sizeof...(Ts)>;
};

§(pack_size)Pack<int, long, char> pack;

template <int Outer> struct Level {
    template <int Inner> struct Nested {
        static constexpr int outer = Outer;
    };
};

Level<1>::§(outer_parameter)Nested<2> nested;

template <int Size> struct Doubled {
    static constexpr int capacity = Size * 2;
};

§(dependent_initializer)Doubled<4> doubled;
}
