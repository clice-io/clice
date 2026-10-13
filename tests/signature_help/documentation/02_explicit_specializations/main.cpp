/// Parses a value written in the given base.
template <typename T>
T parse(T base, const char* text);

/// Parses an integer, its digits in the given base.
template <>
int parse<int>(int radix, const char* digits);

int main() {
    parse(16.0, §(primary)"ff");
    parse(16, §(specialization)"ff");
}
