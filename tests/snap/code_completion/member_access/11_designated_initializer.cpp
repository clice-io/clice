/// # Designated initializer fields
///
/// - status: supported
/// - diagnostics: expected
///
/// `{ .` lists the fields of the aggregate being initialized, those of a
/// matching partial specialization for a dependent one

// The designators dangle; the statements stay semicolon-terminated so a
// later marker is not dragged into recovery.
struct Point {
    int x;
    int y;
    void reset();
};

template <typename T>
struct Options {
    int width;
    int height;
};

template <typename T>
struct Options<T*> {
    int stride;
    void reset();
};

template <typename T>
struct Options<T**> {
    void reset();
};

template <typename T>
void bar() {
    Point p = { .§(plain) };
    Options<T*> o = { .§(partial) };
    Options<T**> n = { .§(no_fields) };
}
