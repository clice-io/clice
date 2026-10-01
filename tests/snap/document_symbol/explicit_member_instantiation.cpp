// - verify: both
//
// An explicit instantiation of a class template's member class lists like
// any explicit instantiation, at its directive; the instantiated member
// class itself never shows up as a declaration of its own.

template <class T>
struct Outer {
    struct Inner {
        int k;
        void f() {}
    };
};

template struct Outer<int>::Inner;
