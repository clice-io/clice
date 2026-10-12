int outer(int value);
int fail(const char* message);
int print(const char* format, ...);

#define CHECK(cond, msg) ((cond) ? 0 : fail(msg))
#define TRACE(format, args...) print(format, args)

int around = outer(CHECK(§(inside_call)1 > 0, "m"));
int grouped = CHECK((§(parenthesized)1 > 0), "m");
int named = TRACE("%d", §(named_variadic)1);

int main() {
    auto square = [](int n) { return n * n; };
    return CHECK(square(§(lambda)3) > 0, "m");
}
