/// # `using enum`
///
/// - status: supported
/// - issues: clangd#1283
///
/// Using declarations highlight enum names at the using site

enum class Color { Red };

void paint() {
    using enum §Color;
    auto c = §Red;
}
