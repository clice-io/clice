// - diagnostics: expected

// A defaulted value parameter computed from another template's static member
// still takes part in partial specialization matching, for a member access
// and a scope alike.
struct ColumnMajor {};

template <typename T>
struct Config {
    static const int stages = 2;
};

template <typename A, typename L, int Stages = Config<A>::stages>
struct Gemm {
    int primary_only;
};

template <typename A, int Stages>
struct Gemm<A, ColumnMajor, Stages> {
    int partial_only;
    static const int partial_stages = Stages;
};

template <typename E>
void bar() {
    using G = Gemm<E, ColumnMajor>;
    G g;
    g.§(member);
    G::§(scope);
}
