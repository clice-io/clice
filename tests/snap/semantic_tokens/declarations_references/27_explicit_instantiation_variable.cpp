/// # Variable explicit instantiation directives
///
/// - status: partial
/// - issues: llvm#191658
///
/// Clang builds no node for the directive, so every identifier on it goes unpainted: the name, the template arguments, even the declarator's type

struct Widget {};

template <typename T>
T zero = T();

extern template §Widget §zero<§Widget>;

template §Widget §zero<§Widget>;
