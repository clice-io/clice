struct Point {
    int x;
    int y;
};

struct §(record)Record {
    Record(int id, Point point, const char* label) : id(id), point(point), label(label) {}

    int id;
    Point point;
    const char* label;
};
