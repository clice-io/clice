int anchor = 0;

#define ECHO(x) x
#define INNER_VAL 99

int nested = ECHO(§(nested_arg)INNER_VAL);
