/// # Nested aggregates
///
/// - status: supported
///
/// Written braces recurse; omitted braces flatten into `.outer.inner=`

struct Inner {
    int x;
    int y;
};

struct Outer {
    Inner a;
    Inner b;
};

Outer o{{1, 2}, 3};
