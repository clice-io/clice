/// # Constructor templates
///
/// - status: supported
/// - config: {"bundle_overloads": false}
/// - diagnostics: expected
///
/// A constructor template completes as the bare class name, like any other
/// constructor

// The completion prefix dangles as an unfinished statement.
namespace lib {
template <typename T>
struct Wrapper {
    template <typename U>
    Wrapper(U value);
};
}  // namespace lib

void bar() {
    lib::Wra§(pos);
}
