/// # Implicit Code Navigation
///
/// ## Overloaded operators — from the operator token to its definition
///
/// - status: supported
/// - verify: server
/// - order: 14
///
/// Go-to-definition on an overloaded operator token reaches the operator's
/// definition. The binary, subscript, call and arrow operators (`+`, `[]`,
/// `()`, `->`) are all resolved.

struct Iterator {
    int value;
};

struct Vec {
    Vec operator+(const Vec& other) const;
    int operator[](int index) const;
    int operator()(int a, int b) const;
    Iterator* operator->();
};

void use(Vec a, Vec b) {
    Vec c = a §(plus)+ b;
    int e = a§(subscript)[0];
    int f = a§(call)(1, 2);
    a§(arrow)->value;
}
