/// # Symbol Kinds
///
/// ## Explicit instantiation directives — the class forms appear as childless symbols; clang mislocates the function and variable forms at the pattern, so they are missing from the outline
///
/// - status: partial
/// - issues: llvm#191658
/// - order: 5

template <typename T>
struct Box {
    T value;
};

template struct Box<int>;
extern template struct Box<char>;

template <typename T>
void convert(T value) {}

template void convert<int>(int);

template <typename T>
T zero = T();

template int zero<int>;
