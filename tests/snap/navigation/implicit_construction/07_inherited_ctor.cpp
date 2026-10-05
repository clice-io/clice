/// # Inherited constructors
///
/// - status: supported
/// - verify: server
///
/// An inherited-constructor declaration navigates to every imported base
/// constructor
///
/// Go-to-definition on an inherited-constructor declaration
/// (`using Base::Base;`) lists each constructor of the base it imports.

struct Base {
    Base(int x);
    Base(int x, int y);
};

struct Derived : Base {
    using Base::§(inherit)Base;
};
