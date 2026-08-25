/// # Documentation
///
/// ## Inherited override docs — an override with no comment shows the base method's documentation
///
/// - status: partial
/// - order: 4
/// - issues: clangd#2504
///
/// Hovering an overriding method that carries no comment of its own should
/// surface the documentation from the method it overrides. clice does not
/// inherit it yet — the override's card carries no description.

namespace inherit_docs {
struct Base {
    /// Renders the widget.
    virtual void draw();
};
struct Circle : Base {
    void §(01_override)draw() override;
};
}
