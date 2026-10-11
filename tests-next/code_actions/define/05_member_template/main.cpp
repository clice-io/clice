// A member template keeps its own template head after the class's, without its
// default arguments and with its requires-clause.

template <typename T>
concept Small = sizeof(T) <= 8;

template <typename T>
struct Visitor {
    template <typename U = int>
    void §(visit)visit(U value);

    template <Small V>
        requires(sizeof(V) > 1)
    void §(small)small(V value);
};
