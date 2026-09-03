/// # Nested aggregates — written braces recurse; omitted braces flatten into `.outer.inner=`
///
/// - status: supported

struct Inner {
    int x;
    int y;
};

struct Outer {
    Inner a;
    Inner b;
};

Outer o{{1, 2}, 3};
