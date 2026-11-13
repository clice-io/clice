struct T1 {};

template <class T, class = void>
struct Taken {
    T1 §(name_taken_by_type)f();
};

struct T0 {};

struct Scope {
    template <class>
    struct Inner;
};

template <class>
struct Scope::Inner {
    void §(name_taken_in_member_class)f(T0 value);
};
