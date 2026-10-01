// - verify: inspect
// - flags: ["-std=c++23", "-fms-extensions"]
//
// Names written where the traversal used to look past them: offsetof
// designators, user-defined literals, attribute arguments, asm goto labels,
// the condition of explicit(...), MS properties and a dependent delegating
// constructor.

struct S {
    int first;
    int field;
};

int by_offsetof() {
    return __builtin_offsetof(S, field);
}

int operator""_unit(unsigned long long value);

int literal() {
    return 12_unit;
}

void cleanup_target(int* pointer);

void attributes(int value) {
    int x __attribute__((cleanup(cleanup_target)));
    [[assume(value > 0)]];
}

void jump(int n) {
    asm goto("" : : : : done);
    if(n) {
        goto done;
    }
done:
    return;
}

constexpr bool enabled = true;

struct Explicit {
    explicit(enabled) Explicit(int);
    explicit(enabled) operator bool() const;
};

struct Property {
    int get_value();
    void put_value(int);
    __declspec(property(get = get_value, put = put_value)) int value;
};

int property(Property p) {
    p.value = 1;
    return p.value;
}

template <class T>
struct Delegating {
    Delegating(T);
    Delegating() : Delegating(T{}) {}
};
