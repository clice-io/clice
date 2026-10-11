// Using declarations highlight enum names at the using site.
enum class Color { Red };

void paint() {
    using enum §(Color)Color;
    auto c = §(Red)Red;
}
