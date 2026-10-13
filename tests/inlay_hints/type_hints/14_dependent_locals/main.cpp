// A local `auto` whose initializer depends on a function template's
// parameter hints the type the template's only instantiation gives it,
// references collapsed as there, or the type a class template's member
// declares. Otherwise it hints the type as the template writes it: a
// parameter's default, the only instantiation of a template the
// initializer calls, and a template candidate a call passes over for
// another overload never stand in; a generic lambda's invented parameter
// has no name to write.
struct Point {
    int x;
    int y;
};

template <typename T>
int copy(T value) {
    §(function)⟦auto kept = value;⟧
    return 0;
}

int copied = copy(Point{});

template <typename T>
void forward(T&& value) {
    §(collapsed)⟦auto&& kept = static_cast<T&&>(value);⟧
}

void pass(Point point) {
    forward(point);
}

auto measure = [](const auto& point) {
    §(lambda)⟦auto width = point.x;⟧
    return width;
};

int measured = measure(Point{});

auto echo = [](auto value) {
    §(several)⟦auto kept = value;⟧
    return kept;
};

int first = echo(1);
long second = echo(2L);

template <typename T>
struct Vec {
    int size() const;
};

template <typename T>
int count(const Vec<T>& items) {
    §(uninstantiated)⟦auto total = items.size();⟧
    return total;
}

template <typename T = int>
struct Holder {
    T value;

    void copy() {
        §(defaulted)⟦auto kept = value;⟧
    }
};

struct A {
    int a;
};

struct B {
    int b;
};

template <class T, class... Args>
T* create(Args&&... args);

A* made = create<A>();

template <class U>
void build(U u) {
    §(explicit_arguments)⟦auto* built = create<B>(u);⟧
}

template <typename T>
void place(T value) {
    §(passed_over)⟦auto kept = value;⟧
}

template <typename T>
void place(T* value);

void put(Point* point) {
    place(point);
}
