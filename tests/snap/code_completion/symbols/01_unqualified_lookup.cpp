/// # Unqualified lookup with fuzzy prefix matching — strong prefix matches survive, weak subsequence matches and unqualified namespace members do not
///
/// - status: supported
/// - diagnostics: expected

// The completion expression dangles as an unfinished statement.
namespace A {

void fooooo();

}

struct X {
    void operator()() {}
};

void bar() {
    X functor;
    auto folded = [](int x) {
    };
    fo§(pos);
}
