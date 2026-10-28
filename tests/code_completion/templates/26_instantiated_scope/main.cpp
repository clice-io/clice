// A template the code after the completion point instantiates once: a scope
// named by its parameter, and an argument of function type, `noexcept`
// included.
struct Settings {
    static int retries;
    using value_type = int;
};

template <class F>
struct Callback {
    F* target;
    int calls;
};

template <class F>
struct Guarded {
    int calls;
};

template <class R, class... A>
struct Guarded<R(A...) noexcept> {
    int guarded_calls;
};

template <class T>
int guard(T callback) {
    return callback.§(noexcept_function_type);
}

template <class T>
int read() {
    return T::§(scope);
}

template <class T>
int fire(T callback) {
    return callback.§(function_type);
}

int use() {
    fire(Callback<void(int)>{});
    guard(Guarded<void(int) noexcept>{});
    return read<Settings>();
}
