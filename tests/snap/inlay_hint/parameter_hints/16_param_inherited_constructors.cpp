/// # Inherited constructors — `using Base::Base` calls lose their parameter names
///
/// - status: partial
/// - issues: clangd#1364

struct Base {
    Base(int width);
};

struct Derived : Base {
    using Base::Base;
};

// No `width:` hint yet.
Derived d(7);
