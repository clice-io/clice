// A name written in a macro's arguments has the card of what it names,
// not of the macro.

#define CHECK(cond) ((cond) ? 1 : 0)
#define MAX(a, b) ((a) > (b) ? (a) : (b))

int limit = 4;

int check(int* pointer) {
    int ok = CHECK(§(pointer)pointer != nullptr);
    return MAX(ok, §(variable)limit);
}
