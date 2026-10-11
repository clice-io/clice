template <typename T>
struct Ptr {
    T* operator->();
    T& operator*();
    T* raw;
};

struct §(type)Widget {};

int use(Ptr<Widget> §(ptr)ptr) {
    return 0;
}
