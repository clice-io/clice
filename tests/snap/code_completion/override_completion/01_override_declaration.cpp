/// # Override declarations
///
/// - status: supported
/// - diagnostics: expected
///
/// Inside a derived class, a base class's virtual function completes as a
/// whole override declaration, return type and `override` included

// The completion prefix dangles inside the class body.
struct Shape {
    virtual int draw(int x, int y) const;
};

struct Circle : Shape {
    dr§(pos)
};
