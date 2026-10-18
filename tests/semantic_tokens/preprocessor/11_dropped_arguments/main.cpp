#define FIRST(a, b) a
#define IGNORE(...)

int left();
int right();

int pick() {
    IGNORE(left(), right());
    return FIRST /* the first */ (left(), /* dropped */ right());
}
