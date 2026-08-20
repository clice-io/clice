/// # Declarations & References
///
/// ## Function explicit instantiation directives — clang builds no node for the directive, so every identifier on it goes unpainted: the name, the template arguments and the parameter types
///
/// - status: partial
/// - issues: llvm#191658
/// - order: 26

struct Widget {};

template <typename T>
void convert(T value) {}

extern template void §convert<§Widget>(§Widget);

template void §convert<§Widget>(§Widget);
