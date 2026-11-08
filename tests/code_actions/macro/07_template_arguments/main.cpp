#define WRAP(x) x

template <class T>
struct Box {};

void f() {
    §(nested)WRAP(auto c = Box<Box<int>>{};)
}
