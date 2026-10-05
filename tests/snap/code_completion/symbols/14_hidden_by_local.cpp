/// # Local hiding a function
///
/// - status: supported
/// - diagnostics: expected
///
/// A local that hides a same-named function is the candidate offered, not the
/// function it hides

// The completion prefix dangles as an unfinished statement.
namespace ns {
int maximum(int a, int b);
}

using namespace ns;

void bar() {
    int maximum = 1;
    int v = maxi§(pos);
}
