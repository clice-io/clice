/// # Member function template
///
/// - status: supported
///
/// A member template keeps its own template head, minus its default arguments, after the class's
///
/// Constrained templates keep their requires-clauses, which a definition must
/// repeat.

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
