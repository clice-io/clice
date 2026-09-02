/// # New expression — a new-expression's constructor arguments drive signature help
///
/// - status: supported

struct Node {
    Node(int value, Node* next);
};

int main() {
    Node* n = new Node(§(pos)0, nullptr);
}
