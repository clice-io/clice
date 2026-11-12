// A dependent using declaration's name is unknown until an instantiation;
// one in a base that depends on nothing names the member, through any
// dependent access.
template <typename T>
struct Derived : T {
    using T::§(value_use)value;

    int use() {
        return §(value_use2)value;
    }
};

struct Base {
    void f();
};

template <typename T>
struct Middle : Base {
    using Base::f;
};

template <typename T>
struct Leaf : Middle<T> {
    void g() {
        this->§(member)f();
        Middle<T>::§(qualified)f();
    }
};
