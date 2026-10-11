// this inside a full and a partial specialization.

namespace full_specialization {
template <typename T> class Holder {};
template <> class Holder<long> {
    Holder* current() {
        return thi§(this_specialization)s;
    }
};
}

namespace partial_specialization {
template <typename T, typename U> struct Pair {};
template <typename U> struct Pair<long, U> {
    const Pair* current() const {
        return thi§(this_partial_specialization)s;
    }
};
}
