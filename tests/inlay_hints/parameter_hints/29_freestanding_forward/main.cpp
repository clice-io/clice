namespace std {
template <typename T> T&& forward(T&);
}

template <typename T> T&& pass(T&);

void use() {
    int i = 0;
    int&& forwarded = std::forward(i);
    int&& passed = pass(i);
}
