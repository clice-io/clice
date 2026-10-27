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
