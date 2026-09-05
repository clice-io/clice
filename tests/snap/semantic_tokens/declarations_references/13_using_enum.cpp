/// # `using enum`
///
/// - status: supported
/// - issues: clangd#1283
///
/// The enum name highlighted at the using site

enum class Color { Red };

void paint() {
    using enum §Color;
    auto c = §Red;
}
