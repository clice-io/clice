// A symbol's extent starts at the attributes and the template header
// written before it, the header of a member template's class included.

[[nodiscard]] int checked();

[[deprecated("use checked")]] [[maybe_unused]] int unchecked();

template <typename T>
[[nodiscard]] T made();

template <typename T>
struct Holder {
    template <typename U>
    U as() const;
};

template <typename T>
template <typename U>
U Holder<T>::as() const {
    return U();
}
