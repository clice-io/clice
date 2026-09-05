/// # Designator suppression
///
/// - status: supported
///
/// Written designators and `/*name=*/` comments keep their inits bare

struct Point {
    int a;
    int b;
    int c;
    int d;
    int e;
};

// Mixing written designators with positional inits is a C99 extension
// This is accepted with a warning; only the bare `4` needs help.
Point p{/*a=*/1, .c = 2, /* .d = */ 3, 4};
