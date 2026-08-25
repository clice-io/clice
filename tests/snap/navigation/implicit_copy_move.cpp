/// # Implicit Code Navigation
///
/// ## Copy/move construction and assignment — to the constructor or assignment operator
///
/// - status: partial
/// - verify: server
/// - order: 4
///
/// Go-to-definition on the `=` of an assignment reaches the assignment
/// operator. The `=` that introduces a copy- or move-initialization
/// (`T b = a;`) is initialization syntax rather than an operator call and is
/// not yet resolved.

struct Widget {
    Widget(int v);
    Widget(const Widget& other);
    Widget(Widget&& other);
    Widget& operator=(const Widget& other);
};

void copies(Widget a) {
    Widget b §(copy_eq)= a;
    Widget c §(move_eq)= static_cast<Widget&&>(a);
    b §(assign_eq)= c;
}
