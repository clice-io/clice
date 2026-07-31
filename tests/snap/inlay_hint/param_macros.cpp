/// # Parameter Hints
///
/// ## Macros at call sites — arguments spelled as macros hint; calls generated inside macro bodies do not
///
/// - status: supported
/// - issues: clangd#2620
/// - order: 11

void report(double value);
void plot(double x, double y);
int check(int status);

#define PI 3.14
#define CALL_REPORT() report(2.71)
#define PAIR 1.0, 2.0
#define ASSERT(expr) if(!(expr)) {}

void use() {
    // An object-like macro is still one written argument.
    report(PI);
    // The call only exists inside the macro body.
    CALL_REPORT();
    // One macro covering several arguments has no place to anchor.
    plot(PAIR);
    // Code written as a macro argument keeps its hints.
    ASSERT(check(42) == 0);
}
