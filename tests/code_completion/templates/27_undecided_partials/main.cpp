// Partial specializations chosen by a defaulted value a dependent type
// leaves open, as libc++'s optional chooses its storage by
// is_trivially_destructible: a member every choice reaches alike completes.
// A partial fixing the value is still more specialized than one taking any.
namespace std {

template <class T>
struct is_trivially_destructible {
    static constexpr bool value = __is_trivially_destructible(T);
};

}  // namespace std

template <class T>
struct storage_base {
    T& get();
};

template <class T, bool = std::is_trivially_destructible<T>::value>
struct destruct_base : storage_base<T> {};

template <class T>
struct destruct_base<T, false> : storage_base<T> {
    ~destruct_base();
};

template <class T>
struct Optional : destruct_base<T> {};

template <class T>
struct Wrap {
    T inner;
    int count;
};

template <class T>
void walk(Optional<Wrap<T>> held) {
    held.get().§(shared_member);
}

template <class T, int N>
struct Slot {
    int primary;
};

template <class T, int N>
struct Slot<T*, N> {
    int generic;
};

template <class T>
struct Slot<T*, 0> {
    int zero;
};

template <class U>
void pick() {
    Slot<U*, 0> slot;
    slot.§(fixed_value);
}
