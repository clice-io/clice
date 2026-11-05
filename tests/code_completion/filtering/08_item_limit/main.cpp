int a_lpha;
int alpha_one;
int alpha_two;
int alpha_three;

int x = alpha§(pos);

#define TEN(p) int p##0, p##1, p##2, p##3, p##4, p##5, p##6, p##7, p##8, p##9;
TEN(cap_a) TEN(cap_b) TEN(cap_c) TEN(cap_d) TEN(cap_e) TEN(cap_f)
TEN(cap_g) TEN(cap_h) TEN(cap_i) TEN(cap_j) TEN(cap_k)

int y = cap_§(many);

// The macro displaces the variable of its name: the limit keeps the best of
// what is shown.
int shade, shade_deep;
#define shade 0

int z = shad§(displaced);
