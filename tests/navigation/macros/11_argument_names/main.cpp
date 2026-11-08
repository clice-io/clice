#define §(definition)LIMIT() 10
#define APPLY(f) f()
#define TWICE(f) f() + f()
#define CAT(a, b) a##b

int a = APPLY(§(argument)LIMIT);
int b = TWICE(LIMIT);
int c = §(call)LIMIT();
int d = CAT(LIM, IT)();
