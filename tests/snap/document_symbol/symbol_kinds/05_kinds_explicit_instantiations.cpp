/// # Explicit instantiation directives
///
/// - status: supported
///
/// Class, function and variable explicit instantiations appear as childless
/// symbols

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
