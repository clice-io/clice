template <typename T>
concept Shape = requires(T t) {
    typename T::unit;
    t.draw();
    t.resize(int{}, int{});
};

template <Shape T>
void render(T& widget) {
    widget.§(member);
}

template <Shape T>
void measure() {
    T::§(nested);
}
