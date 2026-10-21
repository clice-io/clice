template <typename T>
struct Node {
    T value;
};

template <typename T>
void walk(Node<T>& node) {
    au§(member)to value = node.value;
}

inline void take(au§(abbreviated)to item) {
    au§(parameter)to copy = item;
}

struct Widget {};

void drive(Node<Widget>& widget) {
    walk(widget);
    take(Widget{});
}
