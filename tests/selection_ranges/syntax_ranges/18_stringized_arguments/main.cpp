#define STRING(x) #x
#define CHECK(condition) ((condition) ? (void)0 : fail(STRING(condition)))

void fail(const char* message);

void check(int count) {
    CHECK(§(argument)count > 0);
}
