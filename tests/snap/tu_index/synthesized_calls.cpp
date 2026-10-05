// - verify: inspect
// - flags: ["-std=c++23", "-fms-extensions"]
//
// Calls that span no written extent: the ones Sema synthesizes for
// `__builtin_invoke` and a bare MS `__noop`. The file keeps its rows.

namespace std {
template <class T>
struct reference_wrapper {
    T* pointer;
    T& get() const {
        return *pointer;
    }
};

template <class T>
reference_wrapper<T> ref(T& value) {
    return {&value};
}
}  // namespace std

struct S {
    void func() {}
};

void call() {
    S s;
    __builtin_invoke(&S::func, std::ref(s));
    __noop;
}
