// Marker-free canary over declaration syntax: the snapshot pins every
// token, so a stray token appearing anywhere in these declarations fails
// the suite even where nobody thought to place a marker.

namespace demo {

enum class Color { Red, Green };

struct Shape {
    int sides;
};

int area(int width, int height);

}  // namespace demo
