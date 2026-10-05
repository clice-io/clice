/// # Base without a default constructor
///
/// - status: supported
///
/// No constructor is generated when a base class needs its own initializer, since the memberwise one initializes fields alone

struct Base {
    Base(int id);
};

struct §(derived)Derived : Base {
    int x;
};
