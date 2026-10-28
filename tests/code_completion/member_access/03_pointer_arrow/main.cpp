// `->` on a pointer completes the pointee's members.
struct Node {
    int value;
    Node* next;
    int compute(int a);
};

void bar() {
    Node* p;
    p->§(pos);
}
