template <typename T>
concept Shape = requires(T t) {
    t.draw();
    t.resize(int{}, int{});
};

template <Shape T>
void render(T& widget) {
    widget.§(member);
}
