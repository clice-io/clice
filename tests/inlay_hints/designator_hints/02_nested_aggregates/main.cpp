struct Inner {
    int x;
    int y;
};

struct Outer {
    Inner a;
    Inner b;
};

Outer o{{1, 2}, 3};
