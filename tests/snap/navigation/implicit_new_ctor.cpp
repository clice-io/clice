/// # Implicit Code Navigation
///
/// ## `new` expression — navigate to the constructor and overloaded `operator new`
///
/// - status: partial
/// - verify: server
/// - order: 8
///
/// Go-to-definition on `new` reaches the class's overloaded `operator new`.
/// The constructor invoked by the same expression is not part of the reply.

struct Pool {
    Pool();
    static void* operator new(decltype(sizeof(0)) size);
};

void make() {
    Pool* p = §(new_kw)new Pool();
}
