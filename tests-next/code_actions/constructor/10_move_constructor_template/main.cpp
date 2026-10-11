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
