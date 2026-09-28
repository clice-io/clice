/// # Destructor labels
///
/// - status: supported
/// - diagnostics: expected
///
/// A destructor completes as `~` and the bare class name, whatever namespace
/// the class lives in

// The completion point dangles after the dot.
namespace bank {
struct Account {
    ~Account();
    int balance;
};
}  // namespace bank

void bar(bank::Account account) {
    int v = account.§(pos);
}
