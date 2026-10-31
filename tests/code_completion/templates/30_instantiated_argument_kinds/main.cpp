// Templates the code after the completion point instantiates once, each with
// an argument of another kind: the class the completion rebuilds from it is
// the one whose members it offers, a specialization by its exact arguments.
struct Holder {
    int held;
};

struct Outer {
    struct Inner {
        int inner_value;
    };
};

namespace {
struct Hidden {
    int hidden_value;
};
}  // namespace

template <class T>
struct Kind {
    int primary;
};

template <>
struct Kind<const Holder> {
    int const_holder;
};

template <>
struct Kind<Holder&&> {
    int moved_holder;
};

template <>
struct Kind<void() const> {
    int const_function;
};

template <>
struct Kind<void() &&> {
    int moved_function;
};

template <int N>
struct Slots {
    int slots;
};

template <>
struct Slots<-1> {
    int negative_slots;
};

template <class... T>
struct Tuple {
    int size;
};

template <>
struct Tuple<int, Holder> {
    int pair_size;
};

template <class T>
struct Box {
    int boxed;
};

template <template <class> class W>
struct Wrap {
    int wrapped;
};

template <>
struct Wrap<Box> {
    int box_wrapped;
};

template <class T>
int point(T p) {
    return p->§(pointer);
}

template <class T>
int first(T& items) {
    return items[0].§(array);
}

template <class T>
int constant(T kind) {
    return kind.§(const_argument);
}

template <class T>
int moved(T kind) {
    return kind.§(rvalue_reference);
}

template <class T>
int method(T kind) {
    return kind.§(const_function);
}

template <class T>
int moved_method(T kind) {
    return kind.§(ref_qualified_function);
}

template <class T>
int count(T slots) {
    return slots.§(integral);
}

template <class T>
int pair(T tuple) {
    return tuple.§(pack);
}

template <class T>
int wrap(T wrapper) {
    return wrapper.§(template_template);
}

template <class T>
int nested(T inner) {
    return inner.§(class_scope);
}

template <class T>
int hidden(T value) {
    return value.§(anonymous_namespace);
}

int use() {
    Holder holder;
    Holder items[3];
    point(&holder);
    first(items);
    constant(Kind<const Holder>{});
    moved(Kind<Holder&&>{});
    method(Kind<void() const>{});
    moved_method(Kind<void() &&>{});
    count(Slots<-1>{});
    pair(Tuple<int, Holder>{});
    wrap(Wrap<Box>{});
    nested(Outer::Inner{});
    return hidden(Hidden{});
}
