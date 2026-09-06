/// # Namespace-qualified lookup
///
/// - status: supported
/// - diagnostics: expected
///
/// `ns::` lists the namespace's own members

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
