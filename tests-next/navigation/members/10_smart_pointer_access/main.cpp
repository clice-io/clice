// A member reached through an overloaded operator-> or operator* is the
// pointee's member; the arrow itself is the operator.

template <typename T>
struct Ptr {
    T* operator->() const;
    T& operator*() const;
};

struct Widget {
    int size;
    int grow(int by);
};

int use(Ptr<Widget> ptr) {
    ptr->§(arrow_member)grow(1);
    return (*ptr).§(deref_member)size + ptr§(arrow)->size;
}
