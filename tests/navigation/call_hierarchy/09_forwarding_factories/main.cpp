template <typename T, typename... Args>
T make(Args&&... args) {
    return T(static_cast<Args&&>(args)...);
}

struct Widget {
    §(constructor)Widget(int w, int h);
};

Widget build() {
    return make<Widget>(800, 600);
}
