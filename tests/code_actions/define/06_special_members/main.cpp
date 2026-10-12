struct Meter {};

struct S {
    §(ctor)S();
    explicit §(explicit_ctor)S(int value);
    §(dtor)~S();
    S& §(assign)operator=(const S& other);
    §(conversion)operator Meter() const;
};
