void report(double value);
void plot(double x, double y);
int check(int status);

#define PI 3.14
#define CALL_REPORT() report(2.71)
#define PAIR 1.0, 2.0
#define ASSERT(expr) if(!(expr)) {}

void use() {
    report(PI);
    CALL_REPORT();
    plot(PAIR);
    ASSERT(check(42) == 0);
}
