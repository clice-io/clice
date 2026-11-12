namespace app {

struct Holder {
    int value;

    Holder() : value{0} {
        value += 1;
    }
};

struct Allman
{
    int x;
    int y;
};

int compute()
{
    int a = 1;
    return a;
}

typedef struct {
    int x;
    int y;
} Point;

int values[] =
{
    1,
    2,
};

}  // namespace app

void unbalanced() {
#if defined(UNBALANCED)
}
#else
}
#endif

void balanced() {
#if defined(BALANCED)
    int a = 1;
#else
    int a = 2;
#endif
}
