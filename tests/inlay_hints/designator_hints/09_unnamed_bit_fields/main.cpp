struct Flags {
    int low;
    int : 4;
    int high;
};

Flags braced{1, 2};
Flags parenthesized(3, 4);

struct Padded {
    int : 8;
    int value;
};

Padded padded{5};
