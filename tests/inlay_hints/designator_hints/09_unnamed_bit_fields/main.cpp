struct Flags {
    int low;
    int : 4;
    int high;
};

Flags braced{1, 2};
Flags parenthesized(3, 4);
