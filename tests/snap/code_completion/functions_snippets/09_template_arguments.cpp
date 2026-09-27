/// # Template argument placeholders
///
/// - status: supported
/// - config: {"enable_template_arguments_snippet": true}
/// - diagnostics: expected
///
/// A class template inserts a placeholder per template parameter without a
/// default

// The completion prefix dangles as an unfinished declaration.
template <typename T, typename Alloc = int>
struct Buffer {};

void bar() {
    Buf§(pos) b;
}
