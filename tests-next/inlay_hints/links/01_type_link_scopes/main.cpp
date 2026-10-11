// A class scope written back in front of a deduced member type links each of
// its names in order, even where a template argument shares the member's name.

struct Widget {};

template <typename T>
struct List {
    struct Widget {};
};

auto entry = [] { return List<Widget>::Widget{}; }();
