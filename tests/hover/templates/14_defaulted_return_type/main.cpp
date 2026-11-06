template <class T>
struct Vec {
    int vec_member;
};

template <class T>
struct Box {
    template <class U = Vec<T>>
    U make();
};

template <class T>
int use(Box<T>& box) {
    return box.make().§(member)vec_member;
}

namespace lib {
struct Text {
    int text;
};

template <class R = Text, class... A>
R convert(A&&... a);

template <class A, class T = Vec<A>>
T pair(A a, T t);
}  // namespace lib

struct Real {
    int real;
};

template <class T>
int explicit_arguments(T x) {
    int packed = lib::convert<Real>(x).§(explicit_pack)real;
    return packed + lib::pair<int>(x, Real()).§(explicit_then_deduced)real;
}

template <class V>
struct Api {
    template <class X = int, class U = char>
    static U select(U u);
    template <class U = lib::Text>
    static U pointed(const U* p);
};

template <class T>
int deduced_after_default(Real* p) {
    int selected = Api<T>::select(Real()).§(deduced_after_default)real;
    return selected + Api<T>::pointed(p).§(qualified_pointer)real;
}
