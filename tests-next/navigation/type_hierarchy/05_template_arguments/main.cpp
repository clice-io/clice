struct Foo {};

struct §(base)Base {};

template <typename T>
struct Derived : Base {};

Derived<Foo> instance;
