/// # Symbols
///
/// ## Namespace-qualified lookup — `ns::` lists the namespace's own members
///
/// - status: supported
/// - order: 10
/// - diagnostics: expected

// The qualified-id is left dangling at the point.
namespace geometry {

int area_of(int r);

struct Point {
    int x;
};

int origin;

}  // namespace geometry

void bar() {
    int v = geometry::§(pos);
}
