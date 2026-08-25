/// # Symbol Information
///
/// ## Virtual modifiers — `virtual` / `override` / `final` show on method hover
///
/// - status: partial
/// - order: 6
/// - issues: clangd#2474
///
/// Modifiers written in the source render (`virtual … = 0`, `override`,
/// `final`), but an overriding method that omits the redundant `virtual`
/// keyword gives no sign of its virtuality — the card lacks the
/// `virtual void draw() override` form the issue asks for.

struct Base {
    virtual void dr§(pure_virtual)aw() = 0;
};

struct Circle : Base {
    void dr§(override_method)aw() override;
};

struct Dot final : Circle {
    void dr§(final_method)aw() final;
};
