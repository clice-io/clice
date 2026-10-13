template <typename A, typename B>
struct Pair {
    A first;
    B second;
    Pair(A a, B b);
};

Pair pair(1, 2.5);
auto copy = Pair(3, 'c');
auto* heap = new Pair(4, 5);

template <typename T>
using same = T;

template <typename T>
struct Holder {
    Holder(T value);
};

template <typename T>
Holder(T) -> Holder<same<T>>;

auto held = Holder(6);

template <typename T>
using Twin = Pair<T, T>;

Twin twin(7, 8);

template <typename T>
struct Outer {
    template <typename U>
    struct Inner {
        Inner(U item);
    };
};

Outer<int>::Inner inner(1.5);

template <typename T>
struct Traits {};

template <typename T, typename Tr = Traits<T>>
struct Text {
    Text(const T* source);
};

template <typename T>
Text(const T*) -> Text<T, Traits<T>>;

Text text("abc");
