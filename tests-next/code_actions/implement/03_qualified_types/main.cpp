// Parameter and return types written in the base's scope are qualified so they
// resolve in the derived class; reference qualifiers and constness carry over.

namespace shapes {
struct Color {};
struct Painter {
    struct Options {};
    virtual Color paint(const Options& options) const& = 0;
};
}

namespace app {
struct §(painter)Painter : shapes::Painter {
};
}
