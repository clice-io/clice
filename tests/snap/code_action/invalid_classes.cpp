// - diagnostics: expected

// Classes left incomplete while typing offer no memberwise constructor: a
// base holding a member of incomplete type, one containing itself, and in a
// template, where nothing diagnoses it, a base holding a nested class never
// defined.

struct Incomplete;

struct Partial {
    Incomplete part;
};

struct §(partial_base)OnPartial : Partial {
    int x;
};

struct Recursive {
    Recursive self;
};

struct §(recursive_base)OnRecursive : Recursive {
    int x;
};

struct §(recursive)Again {
    Again self;
    int x;
};

template <typename T>
struct Outer {
    struct Inner;

    struct Holder {
        const Inner inner;
    };

    struct §(undefined_nested)OnHolder : Holder {
        int x;
    };
};
