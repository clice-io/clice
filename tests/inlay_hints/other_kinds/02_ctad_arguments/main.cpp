template <typename A, typename B>
struct Pair {
    A first;
    B second;
    Pair(A a, B b);
};

Pair pair(1, 2.5);
auto copy = Pair(3, 'c');
auto* heap = new Pair(4, 5);
