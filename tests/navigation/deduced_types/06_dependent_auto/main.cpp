template <typename T>
struct Node {
    T value;
    Node* advance();
};

template <typename T>
void walk(Node<T>& node) {
    au§(dependent_call)to next = node.advance();
    au§(dependent_member)to value = node.value;
}

struct Widget {};

void drive(Node<Widget>& node) {
    walk(node);
}
