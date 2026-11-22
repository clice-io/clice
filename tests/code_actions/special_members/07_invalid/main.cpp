// Classes still being typed are ill-formed: neither a class deriving from a
// base that holds a member of incomplete type nor a class containing itself
// gets declarations.

struct Incomplete;

struct Partial {
    Incomplete part;
};

struct §(partial_base)OnPartial : Partial {
    int x;
};

struct §(self_containing)Recursive {
    Recursive self;
    int x;
};
