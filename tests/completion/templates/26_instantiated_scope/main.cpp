// A template the code after the completion point instantiates once: a scope
// named by its parameter, and an argument of function type.
struct Settings {
    static int retries;
    using value_type = int;
};

template <class F>
struct Callback {
    F* target;
    int calls;
};

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
    return read<Settings>();
}
