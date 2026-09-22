/// # Deleted base default constructor
///
/// - status: supported
///
/// A base whose default constructor is deleted, explicitly or by a reference member, blocks the memberwise constructor too

struct Explicit {
    Explicit() = delete;
};

struct Implicit {
    int& ref;
};

struct §(explicit_base)Derived : Explicit {
    int x;
};

struct §(implicit_base)Another : Implicit {
    int x;
};
