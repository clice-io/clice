/// # Special Call Contexts
///
/// ## New expression — a new-expression's constructor arguments drive signature help
///
/// - status: supported
/// - order: 13

struct Node {
    Node(int value, Node* next);
};

int main() {
    Node* n = new Node(§(pos)0, nullptr);
}
