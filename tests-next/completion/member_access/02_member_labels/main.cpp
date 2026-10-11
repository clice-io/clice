// Fields, methods, the destructor and operators complete with plain names:
// the destructor as `~` and the bare class name (never `~struct Account`,
// whatever namespace or template it is in), `operator=` with no space
// before `=`, and a conversion operator spelling its target type.

namespace plain {

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
    acc.§(conversion);
}

}  // namespace plain

namespace templated {

template <typename T>
struct Box {
    T value;
};

void bar() {
    Box<int> b;
    b.§(instantiated);
}

}  // namespace templated

namespace bank {

struct Account {
    ~Account();
    int balance;
};

}  // namespace bank

void bar(bank::Account account) {
    int v = account.§(namespaced);
}
