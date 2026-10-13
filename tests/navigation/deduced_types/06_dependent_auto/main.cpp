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

template <typename T>
void visit(Node<T>& node) {
    au§(several_instantiations)to value = node.value;
}

struct Widget {};

struct Gadget {};

void drive(Node<Widget>& widget, Node<Gadget>& gadget) {
    walk(widget);
    visit(widget);
    visit(gadget);
}

template <typename T>
struct Holder {
    void reset() {
        au§(class_template)to copy = T();
    }
};

template struct Holder<Gadget>;
