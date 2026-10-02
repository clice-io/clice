/// # Tokens stay apart
///
/// - status: supported
///
/// The expansion is spaced so its tokens merge neither with each other nor with the text written flush against the invocation

#define NEG -
#define DEREF(p) *p
#define PICK(c) (c ? 1 : ::fallback())
#define NOTHING

int fallback();

int f(bool c, int a, int* p) {
    int negated = §(negated)NEG-a;
    int divided = a/§(divided)DEREF(p);
    int picked = §(picked)PICK(c);
    int removed = a/§(removed)NOTHING*p;
    return negated + divided + picked + removed;
}
