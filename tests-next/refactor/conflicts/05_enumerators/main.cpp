enum Mode { fast, slow };
enum class Color { red, green };

int compute() { return fast + static_cast<int>(Color::red); }
