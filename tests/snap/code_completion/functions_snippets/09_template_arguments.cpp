/// # Template argument placeholders
///
/// - status: supported
/// - config: {"enable_template_arguments_snippet": true}
/// - diagnostics: expected
///
/// A class template inserts a placeholder per template parameter without a
/// default, and empty brackets when every parameter has one

// The completion prefixes dangle as unfinished declarations.
template <typename T, typename Alloc = int>
struct Buffer {};

template <typename T = int>
struct Defaulted {};

void bar() {
    Buf§(pos) b;
}

using Alias = Defau§(all_defaulted);
