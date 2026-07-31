/// # Declarations & References
///
/// ## `using enum` — the enum name highlighted at the using site
///
/// - status: supported
/// - issues: clangd#1283
/// - order: 13

enum class Color { Red };

void paint() {
    using enum §Color;
    auto c = §Red;
}
