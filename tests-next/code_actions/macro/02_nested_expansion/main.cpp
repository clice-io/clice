#define TWICE(x) ((x) + (x))
#define QUAD(x) TWICE(TWICE(x))

int f(int n) {
    return §(quad)QUAD(n);
}
