#define NUM_C 12
#define MASK(bits) ((1u << (bits)) - 1)
#define GREETING "hello"
#define DOUBLE(x) ((x) * 2.0)
#define ECHO(x) x
#define BEGIN {
#define SUM(a, b) a + b
#define MESSAGE "always holds"
#define CHECK(x) ((void)(x))
#define ARGS (1, 2)

int value = 3;

int number = §(object_like)NUM_C;
unsigned mask = §(function_like)MASK(4);
const char* text = §(string)GREETING;
double twice = §(not_constant)DOUBLE(value);
int echoed = ECHO(§(in_arguments)NUM_C);
unsigned nested = ECHO(§(nested_call)MASK(3));
int copied = §(argument_alone)ECHO(value);
int product = §(partial_expression)SUM(1, 2) * 3;
static_assert(true, §(unevaluated)MESSAGE);

void check() {
    §(void_expression)CHECK(value);
}

template <typename T> void make() {
    T made §(dependent)ARGS;
}

void run() §(statement)BEGIN }
