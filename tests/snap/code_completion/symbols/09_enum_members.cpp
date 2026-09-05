/// # Enum members
///
/// - status: supported
/// - diagnostics: expected
///
/// A scoped enum lists through `Type::`, an unscoped enumerator completes by
/// bare name

// Both completion prefixes dangle; the statements stay
// semicolon-terminated so the second marker is not dragged into recovery.
enum class Color { Red, Green, Blue };

enum Fruit { Apple, Banana };

void bar() {
    Color c = Color::§(scoped);
    int f = App§(unscoped);
}
