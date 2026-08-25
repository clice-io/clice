/// # Call Hierarchy
///
/// ## Qualified name for member functions
///
/// - status: partial
/// - verify: server
/// - order: 5
///
/// A member function's call hierarchy item is produced, but its name field
/// carries only the bare method name (`draw`), not the qualified
/// `Circle::draw` that would tell it apart from a free function.

struct Circle {
    void §(method)draw();
};

void Circle::draw() {}
