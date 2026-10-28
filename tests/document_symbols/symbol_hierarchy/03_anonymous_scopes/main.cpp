namespace {

int hidden_counter = 0;

}  // namespace

static union {
    int global_int;
    float global_float;
};

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

}  // namespace misc
