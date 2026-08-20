/// # Symbol Kinds
///
/// ## Core symbol kinds — namespaces, classes, structs, unions, enums and their members, functions, variables, fields, structured bindings and lambdas all appear in the outline with a mapped LSP symbol kind
///
/// - status: supported
/// - order: 1

namespace kinds {

union Value {
    int i;
    float f;
};

enum Flags { FlagA, FlagB };

enum class Mode : unsigned char { Fast, Safe };

struct Pair {
    struct Meta {
        int tag;
    };

    int first;
    int second;
    static int instances;
};

Pair make_pair();

auto [bound_first, bound_second] = make_pair();

auto lambda = [](int x) {
    return x * 2;
};

}  // namespace kinds
