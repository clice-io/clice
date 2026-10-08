/// # `auto` in uninstantiated templates
///
/// - status: supported
/// - verify: server
///
/// Inside a template, an `auto` whose initializer depends on a template
/// parameter reaches the type the initializer resolves to on the class
/// template

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
