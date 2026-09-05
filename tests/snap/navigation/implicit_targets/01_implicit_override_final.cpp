/// # `override` / `final`
///
/// - status: unsupported
///
/// Navigate to the overridden base method
///
/// Go-to-definition on the `override` or `final` specifier should reach the
/// base class virtual method it overrides; today it returns nothing.

struct Base {
    virtual void draw();
    virtual void paint();
};

struct Derived : Base {
    void draw() override;  // go-to-def on override → Base::draw
    void paint() final;    // go-to-def on final → Base::paint
};
