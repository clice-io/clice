/// # Template arguments in hierarchy
///
/// - status: partial
/// - issues: clangd#31
/// - verify: server
///
/// A subtype produced by a class template specialization is listed, but its
/// item name carries only the bare template name (`Derived`), without the
/// template arguments that would distinguish `Derived<Foo>`

struct Foo {};

struct §(base)Base {};

template <typename T>
struct Derived : Base {};

Derived<Foo> instance;
