/// # Missing enum cases
///
/// - status: supported
/// - issues: clangd#807
///
/// A switch over an enum receives the enumerators it does not handle, followed by a `break`
///
/// Enumerators sharing a handled value are considered covered.

enum class Color { Red, Green, Blue, Azure = Blue };

int paint(Color color) {
    §(sw)switch (color) {
    case Color::Red:
        return 1;
    }
    return 0;
}
