struct Widget {
    Widget(int width, int height);
};

template <typename T, typename... Args>
T* make(Args&&... args) {
    return new T(static_cast<Args&&>(args)...);
}

Widget* widget = make<Widget>(§(forwarded)800, 600);
