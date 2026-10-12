// A copy constructor whose constraints fail for the field's specialization
// does not make the field copyable: it is taken by value and moved from.

namespace std {
template <class T>
T&& move(T& value);
}

template <class T>
struct Maybe {
    Maybe(Maybe&&) = default;
    Maybe(const Maybe&) requires(sizeof(T) > 8) = default;
};

struct §(constrained)Constrained {
    Maybe<int> maybe;
    int count;
};
