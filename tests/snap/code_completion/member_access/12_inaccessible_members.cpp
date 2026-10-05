/// # Inaccessible members
///
/// - status: supported
/// - diagnostics: expected
///
/// Private and protected members are not offered where they cannot be used

// The completion point dangles after the dot.
class Account {
    int secret;

protected:
    int audit;

public:
    int balance;
};

void bar(Account account) {
    int v = account.§(pos);
}
