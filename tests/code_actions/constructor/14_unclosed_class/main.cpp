// A class still being typed, open at the end of the file, offers no
// constructor and no overrides: nothing can go before its closing brace.
#include <cstddef>

struct Shape {
    virtual void resize(decltype(sizeof 0) size) = 0;
};

struct §(open)Square : Shape {
    decltype(sizeof(int)) side;
