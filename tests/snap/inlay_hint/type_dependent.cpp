/// # Type Hints
///
/// ## Dependent `auto` — deduction inside an uninstantiated template body stays silent
///
/// - status: partial
/// - issues: clangd#2275
/// - order: 9

template <typename T>
void body(T input) {
    // No hint: the deduced type depends on T.
    auto derived = input + 1;
    // A dependence-free initializer still hints normally.
    auto counter = 0;
}
