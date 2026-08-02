/// # Member Access
///
/// ## Inherited members — a derived object completes its own members and those of its base
///
/// - status: supported
/// - order: 12

// error-ok: the member access expression is left dangling at the point.
struct Base {
    int base_field;
    int base_method();
};

struct Derived : Base {
    int derived_field;
};

void bar() {
    Derived d;
    d.§(pos)
}
