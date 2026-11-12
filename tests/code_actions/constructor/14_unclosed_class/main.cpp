// A class still being typed, open at the end of the file, offers no
// constructor, no overrides and no definition out of line: nothing can go
// before or after its closing brace. A method is still defined in place.
#include <cstddef>

struct Shape {
    virtual void resize(decltype(sizeof 0) size) = 0;
};

struct §(open)Square : Shape {
    decltype(sizeof(int)) side;
    int §(area)area() const;
