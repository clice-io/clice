/// # Missing Symbols
///
/// ## Friend function definitions — a friend function defined inline in a class appears under that class
///
/// - status: supported
/// - order: 6

struct Owner {
    friend void inline_friend(Owner& o) {}

    friend bool operator==(const Owner& lhs, const Owner& rhs) {
        return &lhs == &rhs;
    }
};
