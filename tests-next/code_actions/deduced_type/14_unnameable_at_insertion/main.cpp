// A type the declaration can name is written, a local class or one nested
// in it, and a private type inside its own class; a type it cannot name
// stays auto: a local class of another function, a private type outside its
// class, a type whose spelling needs a lambda or a local name, the type of
// `this`.

template <class T, class Compare>
struct Set {
    struct iterator {};
    iterator begin() { return {}; }
};

template <class T>
struct Box {};

template <class T>
T id(T value);

struct Self {
    auto pointer() -> decltype(this)*;
};

auto local() {
    struct Hidden {};
    return Hidden{};
}

auto local_box() {
    int local = 0;
    return Box<decltype(local)>{};
}

class C {
    struct Private {};
    template <class T>
    struct Item {};

public:
    static Private make();
    static Item<int> make_item();

    void member() {
        §(member)auto p = make();
    }
};

void f() {
    struct Own {
        struct Nested {};
    };
    §(own)auto o = Own{};
    §(nested)auto n = Own::Nested{};
    §(local)auto h = local();
    §(private)auto p = C::make();
    auto less = [](int a, int b) { return a < b; };
    Set<int, decltype(less)> set;
    §(closure)auto it = set.begin();
    §(specialization)auto item = id(C::make_item());
    §(expression)auto box = local_box();
    Self self;
    §(this)auto pointer = self.pointer();
}
