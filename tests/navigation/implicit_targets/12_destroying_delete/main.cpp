// A destroying `operator delete` runs in place of the destructor, which the
// `delete` therefore does not reach.
namespace std {
struct destroying_delete_t {
    explicit destroying_delete_t() = default;
};
}

struct Node {
    ~Node();
    void operator delete(Node* node, std::destroying_delete_t);
};

void dispose(Node* node) {
    §(delete)delete node;
}
