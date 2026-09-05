/// # Inherited constructor docs
///
/// - status: unsupported
/// - issues: clangd#1936
///
/// Inherited constructors have no documentation hover yet
///
/// The name in a `using Base::Base;` declaration resolves to the class rather
/// than an inherited constructor, so the base constructor's documentation has
/// no hover surface.

namespace inherited_ctor {
struct Base {
    /// Constructs from a value.
    Base(int value);
};
struct Derived : Base {
    using Base::§(01_inherited_ctor)Base;
};
}
