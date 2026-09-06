/// # Range-based for
///
/// - status: unsupported
///
/// The range-for colon does not navigate to `begin()` or `end()` yet
///
/// Go-to-definition on the `:` of a range-based for does not reach the
/// `begin()` or `end()` chosen for the range.

struct Iterator {
    int operator*() const;
    Iterator& operator++();
    bool operator!=(const Iterator& other) const;
};

struct Range {
    Iterator begin();
    Iterator end();
};

void use(Range r) {
    for (int x : r) {}  // go-to-def on : → Range::begin / Range::end
}
