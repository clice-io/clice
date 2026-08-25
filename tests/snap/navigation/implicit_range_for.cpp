/// # Implicit Code Navigation
///
/// ## Range-based for — navigate to `begin()` / `end()`
///
/// - status: unsupported
/// - order: 19
///
/// Go-to-definition on the `:` of a range-based for should reach the
/// `begin()` / `end()` chosen for the range; today it returns nothing.

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
