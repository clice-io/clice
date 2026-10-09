/// # Nested macros expand fully
///
/// - status: supported
///
/// A macro whose body invokes other macros expands to the final tokens

#define TWICE(x) ((x) + (x))
#define QUAD(x) TWICE(TWICE(x))

int f(int n) {
    return §(quad)QUAD(n);
}
