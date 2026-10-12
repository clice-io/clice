// The expansion is spaced so its tokens merge neither with each other nor
// with the text written flush against the invocation. The static_asserts
// check the meaning after the edit.

#define NEG -
#define DEREF(p) *p
#define PICK(c) (c ? 1 : ::fallback())
#define NOTHING

constexpr int fallback() {
    return 2;
}
constexpr int value = 4;
constexpr const int* pointer = &value;

static_assert(§(negated)NEG-value == 4);
static_assert(12/§(divided)DEREF(pointer) == 3);
static_assert(§(picked)PICK(false) == 2);
static_assert(12/§(removed)NOTHING*pointer == 3);
