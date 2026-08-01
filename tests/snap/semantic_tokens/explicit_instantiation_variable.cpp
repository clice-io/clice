/// # Declarations & References
///
/// ## Variable explicit instantiation directives — clang builds no node for the directive, so every identifier on it goes unpainted: the name, the template arguments, even the declarator's type
///
/// - status: partial
/// - issues: llvm#191658
/// - order: 27

struct Widget {};

template <typename T>
T zero = T();

extern template §Widget §zero<§Widget>;

template §Widget §zero<§Widget>;
