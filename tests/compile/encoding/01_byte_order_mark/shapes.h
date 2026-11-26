/// A size on the plane.
struct §(extent)Extent {
    int width;
    int height;
};

inline int area(Extent extent) {
    return extent.width * extent.height;
}
