/// # Go to Type Definition
///
/// ## Structured binding variables
///
/// - status: supported
/// - verify: server
/// - order: 6
///
/// Go-to-type-definition on a structured binding reaches the definition of
/// the bound member's type.

struct §(type)Widget {};

struct Pair {
    Widget first;
    int second;
};

Pair make_pair();

int use() {
    auto [§(bound)widget, count] = make_pair();
    return 0;
}
