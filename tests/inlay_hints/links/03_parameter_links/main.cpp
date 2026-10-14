// A parameter hint links to the declaration its name comes from: the
// definition's for a declaration that leaves it unnamed, the underscored
// spelling it strips, the callee a pack forwards to, and a parameter after
// a pack.
void resize(int, int);
void fill(int __value);

template <typename... Args>
void wrap(Args... args) {
    resize(args...);
}

int spread(auto first, auto... rest, auto last) {
    return 0;
}

void use() {
    resize(800, 600);
    fill(1);
    wrap(3, 4);
    int c = spread<int, char>(1, 'x', 2);
}

void resize(int width, int height) {}
