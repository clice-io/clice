/// # Lexical Tokens
///
/// ## Literal prefixes and suffixes — encoding prefixes, type suffixes, digit separators and UDL suffixes as distinct tokens
///
/// - status: unsupported
/// - order: 6

using size_type = decltype(sizeof(0));
constexpr size_type operator""_kb(unsigned long long n) {
    return n * 1024;
}

auto wide = L"wide string";
auto utf8 = u8"utf-8 string";
auto hex = 0xFF;
auto binary = 0b1010;
auto unsigned_suffix = 42u;
auto float_suffix = 3.14f;
auto separators = 1'000'000;
auto udl = 4_kb;
