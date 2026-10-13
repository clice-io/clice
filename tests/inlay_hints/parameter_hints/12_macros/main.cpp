void report(double value);
void plot(double x, double y);
int check(int status);

#define PI 3.14
#define CALL_REPORT() report(2.71)
#define PAIR 1.0, 2.0
#define ASSERT(expr) if(!(expr)) {}

void outer_call(int first, int second, int third);
int inner_call(int left, int right, int last);
#define OUTER(arg) outer_call(0, 0, arg)
#define INNER(arg) inner_call(0, 0, arg)
#define SWAPPED(second, first) plot(first, second)

void use() {
    report(PI);
    CALL_REPORT();
    plot(PAIR);
    ASSERT(check(42) == 0);
    OUTER(INNER(3));
    SWAPPED(4.0, 5.0);
}
