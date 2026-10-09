/// # `override` / `final`
///
/// - status: unsupported
///
/// `override` and `final` do not navigate to the overridden base method yet
///
/// Go-to-definition on the `override` or `final` specifier does not reach the
/// base class virtual method it overrides.

struct Base {
    virtual void draw();
    virtual void paint();
};

struct Derived : Base {
    void draw() override;  // go-to-def on override → Base::draw
    void paint() final;    // go-to-def on final → Base::paint
};
