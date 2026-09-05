/// # Inherited constructor docs
///
/// - status: unsupported
/// - issues: clangd#1936
///
/// `using Base::Base;` surfaces the base constructor's documentation
///
/// A constructor pulled in with `using Base::Base;` should carry the base
/// constructor's documentation on hover. There is no hover surface for it:
/// the name in the using-declaration resolves to the class, not the
/// inherited constructor.

namespace inherited_ctor {
struct Base {
    /// Constructs from a value.
    Base(int value);
};
struct Derived : Base {
    using Base::§(01_inherited_ctor)Base;
};
}
