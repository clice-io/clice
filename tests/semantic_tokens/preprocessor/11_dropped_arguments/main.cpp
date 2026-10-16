#define FIRST(a, b) a
#define IGNORE(...)

int left();
int right();

int pick() {
    IGNORE(left(), right());
    return FIRST(left(), right());
}
