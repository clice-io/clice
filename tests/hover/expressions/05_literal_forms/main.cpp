// Every literal has a card: a character, a string, a floating, an imaginary
// and an integer one, both halves of a user-defined literal, the digits and
// the suffix; so do sizeof and alignof.

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
