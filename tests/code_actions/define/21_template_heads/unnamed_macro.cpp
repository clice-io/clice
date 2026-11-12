#define T0 int

template <class>
struct Macro {
    void §(name_taken_by_macro)f();
};

#undef T0
