/// # Parameter Hints
///
/// ## Mutable reference markers — `&` flags arguments passed by non-const lvalue reference
///
/// - status: supported
/// - issues: clangd#1123
/// - order: 4

void mutate(int& value);
void observe(const int& value);
void take(int&& value);

void use() {
    int v = 0;
    mutate(v);
    observe(v);
    take(static_cast<int&&>(v));
}
