// Templates the code after the completion point instantiates only through
// a macro, and a member access whose call is written after the point.
struct Holder {
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

#define INSPECT(x) inspect(x)
#define CALL(x) call(x)

int use() {
    return INSPECT(Holder{}) + CALL(Holder{});
}
