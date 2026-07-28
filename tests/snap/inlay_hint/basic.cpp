// Seed case for the per-feature snapshot corpus: parameter and type hints.
namespace demo {

int clamp(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

void test() {
    auto result = clamp(42, 0, 10);
    auto& ref = result;
    [[maybe_unused]] auto copy = ref;
}

}  // namespace demo
