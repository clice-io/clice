/// # Variable explicit instantiation directives
///
/// - status: partial
/// - issues: llvm#191658
///
/// Identifiers in a variable explicit-instantiation directive remain unpainted

struct Widget {};

template <typename T>
T zero = T();

extern template §Widget §zero<§Widget>;

template §Widget §zero<§Widget>;
