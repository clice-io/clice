// An anonymous union at global scope folds like one in a namespace.

int before = 0;

static union {
    int u1;
    float u2;
};

namespace wrap {

static union {
    int v1;
    float v2;
};

}  // namespace wrap
