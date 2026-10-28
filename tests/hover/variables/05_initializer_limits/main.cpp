// An initializer can be long in a single token: the definitions of an
// `#embed`ded array and of a long string literal leave it out, and the
// values show the first elements only. One of more than 100 expressions
// renders truncated too, even though it prints short: 128 elements print in
// under 500 characters.

using byte = unsigned char;

static const byte §(embedded)embedded[] = {
#embed "data.txt"
};

const char §(literal)literal[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";

#define DUPLICATE_FOUR(x) x, x, x, x
#define DUPLICATE_64(x) DUPLICATE_FOUR(DUPLICATE_FOUR(DUPLICATE_FOUR(x)))
int val§(token_limit)ues[] = {DUPLICATE_64(3), DUPLICATE_64(3)};

// A short list stays, however many members it leaves out.
#define FOUR_FIELDS(p) int p##0, p##1, p##2, p##3;
#define SIXTEEN_FIELDS(p) FOUR_FIELDS(p##0) FOUR_FIELDS(p##1) FOUR_FIELDS(p##2) FOUR_FIELDS(p##3)
struct Wide {
    SIXTEEN_FIELDS(a) SIXTEEN_FIELDS(b) SIXTEEN_FIELDS(c) SIXTEEN_FIELDS(d)
    SIXTEEN_FIELDS(e) SIXTEEN_FIELDS(f) SIXTEEN_FIELDS(g) SIXTEEN_FIELDS(h)
};
Wide wi§(empty_list)de{};

// A lambda's body prints as `{ ... }`, however much it holds.
#define SUM_FOUR(x) x + x + x + x
#define SUM_SIXTEEN(x) SUM_FOUR(x) + SUM_FOUR(x) + SUM_FOUR(x) + SUM_FOUR(x)
auto su§(lambda_body)m = [](int x) { return SUM_SIXTEEN(x) + SUM_SIXTEEN(x) + SUM_SIXTEEN(x) + SUM_SIXTEEN(x); };
