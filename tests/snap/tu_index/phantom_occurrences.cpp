// - verify: inspect
//
// Declarations without a written name of their own record no occurrence:
// a structured binding's holder (the bindings spell the names), unnamed
// parameters and template parameters. A constrained template parameter is
// one definition, not a definition plus the constraint's implicit use; the
// `auto` of an abbreviated template references the parameter it invents.

struct Pair {
    int first;
    int second;
};

int use() {
    auto [left, right] = Pair{};
    return left + right;
}

template <class T>
int pack(T source) {
    auto [... elements] = source;
    return (elements + ...);
}

template <template <class> class TT>
struct Holder;

auto lambda = []<class T>(T) -> int { return 0; };

int f(unsigned);
int g(int, char*);

template <class T>
concept Concept = true;

template <Concept T>
void constrained(T value);

void abbreviated(Concept auto value);
