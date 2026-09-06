/// # Inherited override docs
///
/// - status: partial
/// - issues: clangd#2504
///
/// An override with no comment does not inherit the base method's documentation
/// yet
///
/// An overriding method with no comment of its own has no description on its
/// hover card.

namespace inherit_docs {
struct Base {
    /// Renders the widget.
    virtual void draw();
};
struct Circle : Base {
    void §(01_override)draw() override;
};
}
