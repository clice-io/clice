// A class moved only through a constructor template taking it by rvalue
// reference, as MSVC's standard library writes unique_ptr's, still moves:
// the field is taken by value and moved from.

namespace std {
template <class T>
T&& move(T& value);
}

template <class T>
struct Unique {
    template <class U = T, int = 0>
    Unique(Unique&& other);
    Unique(const Unique&) = delete;
};

struct §(templated)Templated {
    Unique<int> unique;
    int count;
};

struct Pinned {
    Pinned(const Pinned&) = delete;
    template <class U = int>
        requires(sizeof(U) > 64)
    Pinned(Pinned&&);
};

struct Stuck {
    Stuck(const Stuck&) = delete;
    template <class U>
    Stuck(Stuck&&);
};

struct §(constrained_move)OnPinned {
    Pinned pinned;
    int count;
};

struct §(undeducible_move)OnStuck {
    Stuck stuck;
    int count;
};
