/// # `new` expression
///
/// - status: partial
/// - verify: server
///
/// `new` navigates to an overloaded allocation function but not the constructor
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
