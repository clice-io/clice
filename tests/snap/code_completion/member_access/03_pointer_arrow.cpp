/// # Pointer member access
///
/// - status: supported
/// - diagnostics: expected
///
/// `->` on a pointer completes the pointee's members

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
