/// # Declarations & References
///
/// ## Function and variable explicit instantiation directives — clang mislocates the directive at the pattern, leaving the instantiated name unpainted
///
/// - status: partial
/// - issues: llvm#191658
/// - order: 26

template <typename T>
void convert(T value) {}

template void §convert<int>(int);

template <typename T>
T zero = T();

template int §zero<int>;
