/// # Anonymous and inline scopes
///
/// - status: supported
///
/// Anonymous namespaces, unnamed structs and unions group their members under a placeholder name; inline namespace members stay under the inline namespace node

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

}  // namespace misc
