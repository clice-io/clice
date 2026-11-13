// A nested class's member is defined after the outermost enclosing class,
// qualified through every level.

struct Outer {
    struct Inner {
        void §(run)run();
    };
    int x;
};

struct Holder {
    struct Part;
};

struct Holder::Part {
    void §(defined_apart)attach();
};
