/// # Override declarations
///
/// - status: supported
/// - diagnostics: expected
///
/// Inside a derived class, a base class's virtual function completes as a
/// whole override declaration, return type and `override` included; inside
/// the override, the name completes as itself, not as a call of the base
/// version

// The completion prefixes dangle inside the class body and the override.
struct Shape {
    virtual int draw(int x, int y) const;
};

struct Circle : Shape {
    dr§(pos)
};

struct Square : Shape {
    int draw(int x, int y) const override {
        return dra§(inside_override);
    }
};
