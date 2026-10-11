int fail(const char* message);

#define CHECK(cond, msg) ((cond) ? 0 : fail(msg))

int verified = CHECK(§(condition)1 > 0, "positive");
