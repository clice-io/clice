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

void use(Record& record) {
    Record copy = record;
    Record moved = static_cast<Record&&>(copy);
    copy = moved;
    moved = static_cast<Record&&>(copy);
}
