/// # Member Access
///
/// ## Members of a class — fields, methods, the destructor and operators complete with plain names
///
/// - status: supported
/// - order: 1
/// - diagnostics: expected
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
