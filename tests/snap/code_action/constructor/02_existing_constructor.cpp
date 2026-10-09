/// # Existing constructor not duplicated
///
/// - status: supported
///
/// No constructor is generated when the class already declares one taking as many arguments as it has fields

struct §(point)Point {
    Point(int x, int y);
    int x;
    int y;
};
