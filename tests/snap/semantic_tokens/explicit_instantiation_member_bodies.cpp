/// # Declarations & References
///
/// ## Explicit instantiation member bodies — a dependent name paints as its actual resolution, and as a conflict when the instantiations disagree
///
/// - status: supported
/// - order: 28

struct A {
    static void hit();
};

struct B {
    static int hit;
};

template <typename T>
struct D {
    void go() {
        (void)§T::§hit;
    }
};

template struct D<A>;
template struct D<B>;
