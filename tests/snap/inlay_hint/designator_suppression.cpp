/// # Designator Hints
///
/// ## Designator suppression — written designators and `/*name=*/` comments keep their inits bare
///
/// - status: supported
/// - order: 4

struct Point {
    int a;
    int b;
    int c;
    int d;
    int e;
};

// Mixing written designators with positional inits is a C99 extension
// clang accepts with a warning; only the bare `4` needs help.
Point p{/*a=*/1, .c = 2, /* .d = */ 3, 4};
