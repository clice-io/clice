/// # Call Hierarchy
///
/// ## Follow virtual dispatch
///
/// - status: unsupported
/// - order: 6
///
/// Incoming calls of a base virtual method do not include calls made
/// through derived overrides; a call to an override is attributed only to
/// that override, never to the base it overrides.

struct Base {
    virtual void draw();
};

struct Derived : Base {
    void draw() override;
};

void call_derived(Derived& d) {
    d.draw();  // absent from the incoming calls of Base::draw
}
