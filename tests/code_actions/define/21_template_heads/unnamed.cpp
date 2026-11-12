template <typename T, typename = void>
struct Unnamed {
    void §(unnamed_after_named)f();
};

template <class>
struct Outer {
    template <class>
    struct Inner {
        void §(unnamed_nested)f();
    };
};
