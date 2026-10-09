/// # Variable explicit instantiation directives
///
/// - status: supported
///
/// Identifiers in a variable explicit-instantiation directive are painted

struct Widget {};

template <typename T>
T zero = T();

extern template §Widget §zero<§Widget>;

template §Widget §zero<§Widget>;
