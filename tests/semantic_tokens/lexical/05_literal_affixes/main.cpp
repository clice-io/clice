using size_type = decltype(sizeof(0));
constexpr size_type operator""_kb(unsigned long long n) {
    return n * 1024;
}

auto wide = §(wide_prefix)L"wide string";
auto utf8 = §(utf8_prefix)u8"utf-8 string";
auto hex = 0xFF;
auto binary = 0b1010;
auto unsigned_suffix = 42§(unsigned_suffix)u;
auto float_suffix = 3.14§(float_suffix)f;
auto hex_float_suffix = 0x1p4§(hex_float_suffix)f;
auto separators = 1'000'000;
auto exponent_separators = 1e1'0§(exponent_separator_suffix)f;
auto udl = 4§(literal_operator_suffix)_kb;
