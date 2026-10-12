// The C idiom of a struct named by its typedef: one struct, one name.

typedef struct {
    int x;
    int y;
} Point;

typedef struct Node {
    struct Node* next;
} Node;
