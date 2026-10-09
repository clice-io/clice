/// # CTAD
///
/// - status: supported
/// - verify: server
///
/// A CTAD construction navigates to the deduced specialization's constructor
///
/// When class template argument deduction picks a specialization, go-to-
/// definition on the constructor call reaches the constructor that was
/// selected, not merely the class template.

template <typename T>
struct Box {
    Box(T input) : value(input) {}
    T value;
};

template <typename T>
Box(T) -> Box<T>;

void use() {
    Box b§(ctad_paren)(7);
}
