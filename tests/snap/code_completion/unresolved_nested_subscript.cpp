// - diagnostics: expected

// A subscript on a bare template parameter evaluates to nothing; a member
// access after a second subscript on it completes nothing.
template <typename T>
void bar(T value) {
    value[0][1].§(pos);
}
