static_assert(true);

#define VAR(X) int X = 1;

VAR(from_argument)

#define COUNTER() int counter_from_body = 0;

COUNTER()
