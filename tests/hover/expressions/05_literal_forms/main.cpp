// Every literal has a card: a character, a string, a floating, an imaginary
// and an integer one, both halves of a user-defined literal, the digits and
// the suffix, a static_assert's message, which has no type, one longer than
// the title shows; so do sizeof and alignof.

auto delimiter = '§(char_literal):';
auto greeting = §(string_literal)"welcome";

void measure() {
    (void)size§(sizeof_expr)of(long);
    (void)align§(alignof_expr)of(double);
}

int operator""_unit(unsigned long long value);

int literal() {
    return 1§(udl_digits)2_unit + 12_un§(udl_suffix)it;
}

auto decimal = §(float_literal)3.5;
auto imaginary = §(imaginary_literal)6.0i;
auto integer = §(int_literal)99;

static_assert(sizeof(int) == 4, §(unevaluated_string)"int is four bytes");

auto long_text = §(long_text)"中文中文中文中文中文中文中文中文";

constexpr _Complex double §(negative_imaginary)conjugate = 1.0 - 2.0i;
