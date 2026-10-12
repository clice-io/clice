namespace geo {
// geo holds the shapes.
inline int area(int side) { return side * side; }
}  // namespace geo

const char* label = "geo::area";

int main() { return geo::area(2); }
