/// # Deleted base default constructor
///
/// - status: supported
///
/// A base whose default constructor is deleted, explicitly, by a reference member or by a const member nothing initializes, blocks the memberwise constructor too
///
/// A const member of a class that initializes all its own fields leaves
/// the base default-constructible.

struct Explicit {
    Explicit() = delete;
};

struct Implicit {
    int& ref;
};

struct Point {
    int x;
};

struct Frozen {
    const Point origin;
};

struct Clock {
    int ticks = 0;
};

struct Timed {
    const Clock clock;
};

struct §(explicit_base)Derived : Explicit {
    int x;
};

struct §(implicit_base)Another : Implicit {
    int x;
};

struct §(const_base)Third : Frozen {
    int x;
};

struct §(const_initialized)Fourth : Timed {
    int x;
};
