// A parameter list folds between its own parentheses: not from the name of
// `operator()`, and not through a member-initializer list, a requires clause
// or a `noexcept(...)` after it.

struct Functor {
    int operator()(
        int first,
        int second
    ) const {
        return first + second;
    }
};

struct Pair {
    int x;
    int y;

    Pair(int a,
         int b)
        : x(a),
          y(b) {}
};

void guarded(int a,
             int b) noexcept(sizeof(int) >
                             2) {}

template <typename T>
void constrained(T a,
                 T b)
    requires(sizeof(T) >
             1)
{}

auto lambda = [](int a,
                 int b) noexcept(sizeof(int) >
                                 2) {
    return a + b;
};
