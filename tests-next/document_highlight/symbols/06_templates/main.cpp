// An explicit or partial specialization is a symbol apart from its primary
// template. A member reached through a dependent type highlights with the
// members it may name.

template <class §(param)T>
struct §(primary)Box {
    T value;
    T get() const { return value; }
};

template <>
struct §(explicit_spec)Box<bool> {
    bool flag;
};

template <class T>
struct Box<T*> {
    T* pointer;
};

template <class T>
T §(unwrap)unwrap(const Box<T>& box) {
    return box.§(dependent)get();
}

int use() {
    Box<int> number{1};
    Box<bool> toggle{true};
    return unwrap(number) + unwrap<int>(Box<int>{2});
}
