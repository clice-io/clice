// - verify: server
//
// Call edges for calls the source does not spell as a call: deletion runs
// the destructor and the deallocation function, and a dependent call
// reaches what the resolver settles it on. An implicitly declared global
// operator delete has no source to anchor a hierarchy item at.

struct Node {
    void §(dealloc)operator delete(void* memory);
    §(dtor)~Node();
};

struct Plain {};

void destroy(Node* node, Plain* plain) {
    delete node;
    §(global_delete)delete plain;
}

template <typename T>
struct Base {
    void §(hit)hit(int);
};

template <typename T>
struct Runner : Base<T> {
    void run() {
        this->hit(1);
    }
};
