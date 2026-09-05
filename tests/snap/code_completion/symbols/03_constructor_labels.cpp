/// # Constructor labels stay plain
///
/// - status: supported
/// - config: {"bundle_overloads": false}
/// - diagnostics: expected
///
/// Class template constructors and deduction guides complete as the bare class name, never a templated spelling

// The completion prefix dangles as an unfinished statement.
template <typename T, typename U>
struct Bazzz {
    Bazzz() {}

    Bazzz(T x) {}

    Bazzz(T x, U y) {}
};

template <typename T>
Bazzz(T) -> Bazzz<T, int>;

void bar() {
    Ba§(pos)
}
