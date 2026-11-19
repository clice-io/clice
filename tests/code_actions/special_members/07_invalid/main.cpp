// Classes still being typed are ill-formed, and neither a base holding a
// member of incomplete type nor a class containing itself gets declarations.

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
