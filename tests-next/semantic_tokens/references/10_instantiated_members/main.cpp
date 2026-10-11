// A dependent name paints as its actual resolution: agreeing kinds keep the
// modifiers all instantiations share, disagreeing kinds paint a conflict.
struct A {
    static void hit();
};

struct B {
    static int hit;
};

struct C {
    void hit();
};

template <typename T>
struct D {
    void go() {
        (void)§(T)T::§(hit_use)hit;
    }
};

template struct D<A>;
template struct D<B>;

template <typename T>
struct E {
    void probe(T t) {
        t.§(hit_use2)hit();
    }
};

template struct E<A>;
template struct E<C>;
