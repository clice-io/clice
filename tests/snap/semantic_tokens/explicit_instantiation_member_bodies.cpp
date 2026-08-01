/// # Declarations & References
///
/// ## Explicit instantiation member bodies — a dependent name paints as its actual resolution: agreeing kinds keep the modifiers all instantiations share, disagreeing kinds paint a conflict
///
/// - status: supported
/// - order: 28

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
        (void)§T::§hit;
    }
};

template struct D<A>;
template struct D<B>;

template <typename T>
struct E {
    void probe(T t) {
        t.§hit();
    }
};

template struct E<A>;
template struct E<C>;
