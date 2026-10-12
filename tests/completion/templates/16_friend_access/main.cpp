// A friend has the access of the class befriending it: a friend function
// defined in the class or outside it, a friend class and a friend class
// template complete the members of the class's private base, which a
// stranger does not.
template <class T>
struct Base {
    int value;
};

template <class V>
struct Visitor;

template <class T>
struct Wrapper : private Base<T> {
    friend bool operator==(const Wrapper& a, const Wrapper& b) {
        return a.§(defined_inside);
    }

    template <class U>
    friend int peek(const Wrapper<U>& wrapper);

    friend struct Inspector;

    template <class V>
    friend struct Visitor;
};

template <class U>
int peek(const Wrapper<U>& wrapper) {
    return wrapper.§(defined_outside);
}

struct Inspector {
    template <class T>
    int look(const Wrapper<T>& wrapper) {
        return wrapper.§(friend_class);
    }
};

template <class V>
struct Visitor {
    template <class T>
    int look(const Wrapper<T>& wrapper) {
        return wrapper.§(friend_template);
    }
};

template <>
struct Visitor<int> {
    template <class T>
    int look(const Wrapper<T>& wrapper) {
        return wrapper.§(friend_specialization);
    }
};

template <class T>
int stranger(const Wrapper<T>& wrapper) {
    return wrapper.§(not_a_friend);
}
