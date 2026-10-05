// Attributes written on a lambda and user-defined literals hover like
// their counterparts on functions and calls.

auto lambda = [] [[nodi§(lambda_std)scard]] (int x) __attribute__((always_§(lambda_gnu)inline)) {
    return x;
};

int operator""_unit(unsigned long long value);

int literal() {
    return 1§(udl_digits)2_unit + 12_un§(udl_suffix)it;
}
