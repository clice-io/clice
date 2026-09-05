/// # Explicit instantiation directives
///
/// - status: partial
/// - issues: llvm#191658
///
/// Class explicit instantiations appear as childless symbols, while function
/// and variable forms are missing from the outline

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
