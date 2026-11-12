#define §(macro)LIMIT 10

int §(function)twice(int x) {
    return x * 2;
}

int §(comment_in_body)open() { // }
    return 0;
}

struct Holder {
    §(defaulted)Holder() = default;
};

struct §(record)Point {
    int x;
};

namespace §(namespace)app {
int value;
}

int §(variable)values[] = {1, 2};
