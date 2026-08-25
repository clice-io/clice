/// # Implicit Code Navigation
///
/// ## Inherited constructors — navigate to the base constructors brought in by `using`
///
/// - status: partial
/// - verify: server
/// - order: 11
///
/// Go-to-definition on an inherited-constructor declaration
/// (`using Base::Base;`) reaches a base constructor. When the base declares
/// several constructors the reply resolves to one of them rather than
/// listing the whole set.

struct Base {
    Base(int x);
    Base(int x, int y);
};

struct Derived : Base {
    using Base::§(inherit)Base;
};
