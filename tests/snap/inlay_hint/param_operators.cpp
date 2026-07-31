/// # Parameter Hints
///
/// ## Operators and literals — operator syntax and user-defined literals stay bare; member and default member initializers hint
///
/// - status: supported
/// - order: 18

struct S {
    S(int param);
};

void operator+(S lhs, S rhs);

long double operator""_w(long double param);

struct Holder {
    S member;
    S defaulted{3};
    Holder() : member(42) {}
};

void use() {
    S a(1);
    S b(2);
    a + b;
    1.2_w;
}
