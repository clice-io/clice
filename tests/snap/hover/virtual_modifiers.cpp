/// # Symbol Information
///
/// ## Virtual modifiers — `virtual` / `override` / `final` show on method hover
///
/// - status: supported
/// - order: 6
///
/// The rendered definition carries the modifiers (`virtual … = 0`,
/// `override`); clangd tracks this gap as clangd#2474, clice's hover
/// port renders them already.

struct Base {
    virtual void dr§(pure_virtual)aw() = 0;
};

struct Circle : Base {
    void dr§(override_method)aw() override;
};
