// A macro written in another macro's arguments shows what it expands to
// there: once, though the outer macro uses its parameter twice; a
// function-like one with its own arguments; one two levels deep; one whose
// body names another macro. The outer macro still shows its whole expansion.

#define ECHO(x) x
#define TWICE(x) x + x
#define SCALE(v) (v * 2)
#define LIMIT 99
#define DOUBLE_LIMIT LIMIT * 2

int twice = §(outer)TWICE(§(used_twice)LIMIT);
int scaled = ECHO(§(function_like)SCALE(3));
int deep = ECHO(ECHO(§(two_levels)LIMIT));
int chained = ECHO(§(body_macro)DOUBLE_LIMIT);
