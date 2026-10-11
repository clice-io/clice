struct Callback {
    void operator()(int status, int detail) const;
};

void (*handler)(int status, const char* message);

void use() {
    Callback cb;
    cb(1, 2);
    cb.operator()(3, 4);
    handler(0, "ok");
    auto cmp = [](int lhs, int rhs) { return lhs < rhs; };
    cmp(1, 2);
}
