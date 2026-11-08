#define MINUS_ONE - 1
#define ADDRESS(x) & x

int f(int* p);

int g(int x) {
    int y = x §(binary)MINUS_ONE;
    return f(§(unary)ADDRESS(y)) + y;
}
