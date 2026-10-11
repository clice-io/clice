// Declarations as written in everyday code: a trailing return type, a
// ref-qualifier, a defaulted function pointer, unnamed template parameters, a
// member of a class nested in a template, a trailing requires-clause. A
// deleted function and a friend declaration offer no definition.

namespace app::detail {
struct Handle {
    auto §(trailing)size() -> int;
    void §(ref_qualified)release() &&;
    void §(callback)watch(void (*on_change)(int) = nullptr, int every = limit(1, 2));
    void §(deleted)reset() = delete;
    friend void §(friend_function)swap(Handle& left, Handle& right);
    static int limit(int low, int high);
};
}

template <class, int = 0>
struct Tagged {
    void §(unnamed_parameters)tag();
};

template <class T>
struct Tree {
    struct Node {
        void §(nested_in_template)visit();
    };
    void §(requires_clause)compact() requires(sizeof(T) > 1);
};
