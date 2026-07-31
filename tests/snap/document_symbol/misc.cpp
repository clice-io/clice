// Name rendering for entities without a written identifier (anonymous
// namespaces, structs, unions, lambdas), inline namespaces, scoped enums
// and structured bindings.

namespace {

int hidden_counter = 0;

}  // namespace

namespace misc {

inline namespace v1 {

int versioned();

}  // namespace v1

struct Outer {
    struct {
        int anonymous_member;
    };

    union {
        int as_int;
        float as_float;
    };
};

union Value {
    int i;
    float f;
};

enum Flags { FlagA, FlagB };

enum class Mode : unsigned char { Fast, Safe };

struct Pair {
    int first;
    int second;
};

Pair make_pair();

auto [bound_first, bound_second] = make_pair();

auto lambda = [](int x) {
    return x * 2;
};

}  // namespace misc
