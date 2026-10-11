// A signature two bases share is spelled the way the derived class can name
// it: one base writes the parameter with an alias private to it, the other
// with the type itself. The static_assert fails until the action leaves no
// method pure.

class Aliased {
    using Count = int;

public:
    virtual void resize(Count) = 0;
};

struct Plain {
    virtual void resize(int) = 0;
};

struct §(spelled)Both : Aliased, Plain {};

static_assert(!__is_abstract(Both));
