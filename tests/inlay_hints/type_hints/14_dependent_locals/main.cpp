// A local `auto` whose initializer depends on a function template's
// parameter hints the type the template's only instantiation gives it, or
// the type a class template's member declares; a template instantiated
// with several types hints nothing, and neither does a parameter's default
// or the only instantiation of a template the initializer calls.
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
