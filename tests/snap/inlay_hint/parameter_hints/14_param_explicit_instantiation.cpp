/// # Explicit instantiation
///
/// - status: supported
/// - issues: clangd#1034
///
/// An explicit instantiation definition adds no duplicate hints, while its written template arguments hint normally

template <typename T>
void apply(T value) {}

template void apply<int>(int value);

void use() {
    apply(42);
}

int measure(int amount);

template <typename T>
struct Box {};

template struct Box<decltype(measure(7))>;
