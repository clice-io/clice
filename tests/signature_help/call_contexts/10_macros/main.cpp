int fail(const char* message);
int print(const char* format, ...);

#define CHECK(cond, msg) ((cond) ? 0 : fail(msg))
#define LOG(format, ...) print(format, __VA_ARGS__)

int verified = CHECK(§(condition)1 > 0, "positive");
int logged = LOG("%d %d", 1, §(variadic)2);
int nested = CHECK(fail(§(inner)"inner") == 0, "outer");
