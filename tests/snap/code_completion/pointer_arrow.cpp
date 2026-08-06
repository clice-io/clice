/// # Member Access
///
/// ## Pointer member access — `->` on a pointer completes the pointee's members
///
/// - status: supported
/// - order: 10
/// - diagnostics: expected

// The member access expression is left dangling at the point.
struct Node {
    int value;
    Node* next;
    int compute(int a);
};

void bar() {
    Node* p;
    p->§(pos)
}
