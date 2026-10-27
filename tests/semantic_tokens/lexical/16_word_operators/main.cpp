struct Node {
    int value;
};

int sizes() {
    Node* node = §(new)new Node{1};
    auto bytes = §(sizeof)sizeof(Node) + §(alignof)alignof(Node);
    §(delete)delete node;
    return §(cast)static_cast§(cast_open)<int>(bytes);
}
