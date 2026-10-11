// A C file: struct tags, typedefs and enumerators are colored as in C++, and
// `bool` and `true` are the macros <stdbool.h> defines.
#include <stdbool.h>

struct point {
    int x;
    int y;
};

typedef struct point point_t;

enum color { RED, GREEN };

static int count;

bool same(const point_t* a, const struct point* b) {
    enum color c = RED;
    _Bool flag = true;
    return a->x == b->x && c == RED && flag && count;
}
