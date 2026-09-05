/// # Fuzzy unqualified lookup
///
/// - status: supported
/// - diagnostics: expected
///
/// Strong prefix matches survive, weak subsequence matches and unqualified namespace members do not

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
