// What only the code after the completion point instantiates: a template
// called through a macro, one defined before it among them, or another
// template, one a class member declared after its caller among them, a
// dependent type name, a constructor and an operator; and a member access
// whose call is written after the point.
struct Holder {
    using value_type = int;
    /// What the holder keeps.
    int held;
    int get() const;
};

#define CHECK_IT(x) examine(x)

template <class T>
int examine(T value) {
    return value.§(macro_before);
}

int first() {
    return CHECK_IT(Holder{});
}

template <class T>
int inspect(T value) {
    return value.§(through_macro);
}

template <class T>
int call(T value) {
    return value.§(arguments_follow)();
}

template <class T>
int read() {
    typename T::§(dependent_type) local{};
    return local;
}

template <class T>
int inner(T value) {
    return value.§(through_template);
}

template <class T>
int outer(T value) {
    return inner(value);
}

template <class T>
int show(T value) {
    return value.§(through_member);
}

struct Panel {
    int draw() {
        return render(Holder{});
    }

    template <class U>
    int render(U item) {
        return show(item);
    }
};

struct Printer {
    template <class T>
    Printer(T value) {
        value.§(constructor);
    }
};

struct Sink {
    template <class T>
    int operator<<(T value) {
        return value.§(operator_call);
    }
};

#define INSPECT(x) inspect(x)
#define CALL(x) call(x)

int use() {
    Printer printer(Holder{});
    return INSPECT(Holder{}) + CALL(Holder{}) + read<Holder>() + (Sink{} << Holder{}) +
           outer(Holder{});
}
