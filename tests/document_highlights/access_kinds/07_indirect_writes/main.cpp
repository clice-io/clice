// Writing through a pointer, a member chain or a subscript writes the member
// assigned; the pointer, object or array it is reached through is read.

struct Node {
    int §(value)value;
    Node* next;
};

void store(Node* §(node)node, int* §(slot)slot, int (&§(cells)cells)[4]) {
    node->value = 1;
    node->next->value += 2;
    (*node).value = 3;
    *slot = 4;
    (*slot)++;
    cells[0] = 5;
}
