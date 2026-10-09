/// # Function explicit instantiation directives
///
/// - status: supported
///
/// Identifiers in a function explicit-instantiation directive are painted

struct Widget {};

template <typename T>
void convert(T value) {}

extern template void §convert<§Widget>(§Widget);

template void §convert<§Widget>(§Widget);
