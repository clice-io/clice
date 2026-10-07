// An initializer of more than 200 tokens renders truncated even though it
// prints short: 128 elements print in under 500 characters.

#define DUPLICATE_FOUR(x) x, x, x, x
#define DUPLICATE_64(x) DUPLICATE_FOUR(DUPLICATE_FOUR(DUPLICATE_FOUR(x)))
int val§(token_limit)ues[] = {DUPLICATE_64(3), DUPLICATE_64(3)};
