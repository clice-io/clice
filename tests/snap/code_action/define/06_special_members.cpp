/// # Constructors, destructors and operators
///
/// - status: supported
///
/// Special member functions are defined under the class name they are spelled with
///
/// Conversion functions and operators keep their full spelling.

struct Meter;

struct S {
    §(ctor)S();
    explicit §(explicit_ctor)S(int value);
    §(dtor)~S();
    S& §(assign)operator=(const S& other);
    §(conversion)operator Meter() const;
};
