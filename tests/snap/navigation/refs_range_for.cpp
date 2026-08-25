/// # Find References
///
/// ## Implicit references from range-based for loops
///
/// - status: unsupported
/// - order: 3
/// - issues: clangd#1081
///
/// Find references on `begin` reports only its own declaration; the
/// range-based for loop that implicitly calls it is not included among the
/// references.

struct Iterator {
    int operator*() const;
    Iterator& operator++();
    bool operator!=(const Iterator& other) const;
};

struct Range {
    Iterator begin();  // find-refs here omits the range-for below
    Iterator end();
};

void use(Range r) {
    for (int x : r) {
    }
}
