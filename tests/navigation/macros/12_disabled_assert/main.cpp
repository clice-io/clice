#define NDEBUG

#ifdef NDEBUG
#define assert(condition) ((void)0)
#else
#define assert(condition) ((condition) ? (void)0 : __builtin_trap())
#endif

int §(definition)limit = 10;

void check(int value) {
    assert(value < §(use)limit);
}
