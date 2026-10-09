/// # `auto` in uninstantiated templates
///
/// - status: supported
/// - verify: server
///
/// Inside a template, an `auto` whose initializer depends on a template
/// parameter reaches the type the initializer resolves to on the class
/// template
///
/// A type that is the template parameter itself reaches the parameter, however
/// the template is instantiated.

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
