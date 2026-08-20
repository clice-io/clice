/// # Symbols
///
/// ## Using-declaration — a name pulled in with `using` completes unqualified
///
/// - status: supported
/// - order: 13
/// - diagnostics: expected

// The completion prefix dangles as an unfinished statement.
namespace lib {

int helper_fn(int x);

}

using lib::helper_fn;

void bar() {
    int v = help§(pos);
}
