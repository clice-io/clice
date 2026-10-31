// What only the code after the completion point instantiates: a template
// called through a macro, a generic lambda through the variable it
// initializes, a dependent type name; and a member access whose call is
// written after the point.
struct Holder {
    using value_type = int;
    int held;
    int get() const;
};

template <class T>
int inspect(T value) {
    return value.§(through_macro);
}

template <class T>
int call(T value) {
    return value.§(arguments_follow)();
}

auto probe = [](auto value) { return value.§(lambda_variable); };

template <class T>
int read() {
    typename T::§(dependent_type) local{};
    return local;
}

#define INSPECT(x) inspect(x)
#define CALL(x) call(x)

int use() {
    return INSPECT(Holder{}) + CALL(Holder{}) + probe(Holder{}) + read<Holder>();
}
