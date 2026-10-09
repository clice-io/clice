/// # Inherited constructors
///
/// - status: partial
/// - issues: clangd#1364
///
/// `using Base::Base` calls lose their parameter names

struct Base {
    Base(int width);
};

struct Derived : Base {
    using Base::Base;
};

// No `width:` hint yet.
Derived d(7);
