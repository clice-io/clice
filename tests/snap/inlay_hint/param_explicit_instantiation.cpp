/// # Parameter Hints
///
/// ## Explicit instantiation — an explicit instantiation definition adds no duplicate hints
///
/// - status: supported
/// - issues: clangd#1034
/// - order: 14

template <typename T>
void apply(T value) {}

template void apply<int>(int value);

void use() {
    apply(42);
}
