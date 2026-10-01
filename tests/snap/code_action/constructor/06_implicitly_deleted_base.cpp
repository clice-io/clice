/// # Deleted base default constructor
///
/// - status: supported
///
/// A base whose default constructor is deleted, explicitly, by a reference member or by a const member nothing initializes, blocks the memberwise constructor too

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

struct §(explicit_base)Derived : Explicit {
    int x;
};

struct §(implicit_base)Another : Implicit {
    int x;
};

struct §(const_base)Third : Frozen {
    int x;
};
