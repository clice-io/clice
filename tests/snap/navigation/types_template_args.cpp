/// # Type Hierarchy
///
/// ## Template arguments in type hierarchy items
///
/// - status: partial
/// - verify: server
/// - order: 5
/// - issues: clangd#31
///
/// A subtype produced by a class template specialization is listed, but
/// its item name carries only the bare template name (`Derived`), without
/// the template arguments that would distinguish `Derived<Foo>`.

struct Foo {};

struct §(base)Base {};

template <typename T>
struct Derived : Base {};

Derived<Foo> instance;
