/// # Overloads across namespaces
///
/// - status: supported
/// - diagnostics: expected
///
/// Same-named functions from different namespaces in scope bundle into one
/// entry that counts all of them

// The completion prefix dangles as an unfinished statement.
namespace a {
void draw(int x);
}

namespace b {
void draw(double x);
}

using namespace a;
using namespace b;

void bar() {
    dra§(pos);
}
