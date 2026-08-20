/// # Parameter Hints
///
/// ## Function pointers and call operators — indirect calls still name their parameters
///
/// - status: supported
/// - issues: clangd#1734, clangd#1742
/// - order: 7

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
