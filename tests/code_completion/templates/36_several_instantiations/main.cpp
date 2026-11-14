// A template the code instantiates more than once offers the names every
// instantiation has, their overloads each listed: a call before the
// completion point counts, a generic lambda's calls count, from where it is
// written or from the template it is passed to, a branch of an
// `if constexpr` counts the instantiations taking it, and past eight
// instantiations the first eight tell. One whose object is no class leaves
// nothing to share.
struct Vector {
    using size_type = int;
    int size() const;
    int* data();
    int at(int index);
    void push_back(int value);
};

struct Span {
    using size_type = long;
    int size() const;
    int* data();
    int at(long index) const;
    Span first(int count) const;
};

struct Text {
    int size() const;
    const char* c_str() const;
};

template <class T>
int count(T items);

int before() {
    return count(Text{});
}

template <class T>
int count(T items) {
    return items.§(call_before_point);
}

template <class T>
int read(T items) {
    return items.§(overloads_differ);
}

template <class T>
int measure() {
    typename T::§(dependent_scope) size{};
    return size;
}

template <class T>
constexpr bool growable = false;

template <>
constexpr bool growable<Vector> = true;

template <class T>
void fill(T items) {
    if constexpr(growable<T>) {
        items.§(taken_branch);
    } else {
        items.§(other_branch);
    }
}

template <int N>
struct Slot {
    int value;
    int extra;
};

struct Last {
    int value;
};

template <class T>
int pick(T slot) {
    return slot.§(past_eight);
}

template <class T>
int probe(T value) {
    return value.§(non_class);
}

struct Foo {
    void foo();
    int get(int, double = 1.0);
};

struct Bar {
    void bar();
    int get(int);
    int get(int, const char*);
};

template <class F>
void both(F f) {
    f(Foo{});
    f(Bar{});
}

void pass() {
    both([](auto a) { a.§(passed_lambda); });
}

void use() {
    count(Vector{});
    count(Span{});
    read(Vector{});
    read(Span{});
    measure<Vector>();
    measure<Span>();
    fill(Vector{});
    fill(Span{});
    auto size_of = [](auto items) { return items.§(generic_lambda); };
    size_of(Vector{});
    size_of(Text{});
    pick(Slot<1>{});
    pick(Slot<2>{});
    pick(Slot<3>{});
    pick(Slot<4>{});
    pick(Slot<5>{});
    pick(Slot<6>{});
    pick(Slot<7>{});
    pick(Slot<8>{});
    pick(Last{});
    probe(Vector{});
    probe(1);
}
