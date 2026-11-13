template <class T>
concept Small = sizeof(T) <= 4;

template <Small T, Small... Ts>
struct Box {
    void §(constrained_pack)f();
};

namespace limits {
template <class T>
concept Small = sizeof(T) <= 4;
}

template <limits::Small auto V, limits::Small auto... Vs>
struct Value {
    void §(qualified_constraint)f();
};

struct Config;

namespace cfg {
template <class T>
concept Tiny = true;

template <class T, class U>
concept Same = true;

template <Tiny T, Same<int> U>
struct Holder {
    void §(constraint_outside_namespace)f(Config config);
};
}

struct Config {};
