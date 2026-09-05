/// # Using-declaration
///
/// - status: supported
/// - diagnostics: expected
///
/// A name pulled in with `using` completes unqualified

// The completion prefix dangles as an unfinished statement.
namespace lib {

int helper_fn(int x);

}

using lib::helper_fn;

void bar() {
    int v = help§(pos);
}
