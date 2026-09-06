/// # Dependent `auto`
///
/// - status: partial
/// - issues: clangd#2275
///
/// Deduction inside an uninstantiated template body stays silent

template <typename T>
void body(T input) {
    // No hint: the deduced type depends on T.
    auto derived = input + 1;
    // A dependence-free initializer still hints normally.
    auto counter = 0;
}
