/// # Function explicit instantiation directives
///
/// - status: partial
/// - issues: llvm#191658
///
/// Identifiers in a function explicit-instantiation directive remain unpainted

struct Widget {};

template <typename T>
void convert(T value) {}

extern template void §convert<§Widget>(§Widget);

template void §convert<§Widget>(§Widget);
