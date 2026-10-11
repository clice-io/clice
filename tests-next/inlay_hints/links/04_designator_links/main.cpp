// Each field of a designator path links to its declaration, through omitted
// braces and anonymous members alike.
struct Inner {
    int x;
    int y;
};

struct Outer {
    Inner a;
    Inner b;
};

Outer o{{1, 2}, 3};

struct State {
    union {
        struct {
            struct {
                int depth;
            };
        } level;
    };
};

State s{42};
