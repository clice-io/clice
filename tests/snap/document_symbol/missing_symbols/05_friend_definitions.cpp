/// # Friend function definitions
///
/// - status: supported
///
/// A friend function defined inline in a class appears under that class

struct Owner {
    friend void inline_friend(Owner& o) {}

    friend bool operator==(const Owner& lhs, const Owner& rhs) {
        return &lhs == &rhs;
    }
};
