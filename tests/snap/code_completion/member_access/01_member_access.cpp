/// # Members of a class
///
/// - status: supported
/// - diagnostics: expected
///
/// Fields, methods, the destructor and operators complete with plain names
///
/// The destructor completes as `~Account` (never `~struct Account`),
/// `operator=` keeps no space before `=`, and a conversion operator
/// spells its target type.

// The member access expression is left dangling at the point.
struct Wallet {
    int cents;
};

struct Account {
    int balance;
    int bazzzz(int a, int b);
    operator Wallet();
};

void bar() {
    Account acc;
    acc.§(pos)
}
