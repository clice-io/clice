struct Point {
    int x;
    union {
        int tag;
        float weight;
    };
    struct {
        int width, height;
    } size;
    enum { Small, Large } scale;
};

struct §(struct)Point point;

struct ops {
    int (*open)(struct inode*, struct file*);
    const struct item* first;
    int count;
};

struct §(mentions)ops table;

struct Packed {
    char raw[sizeof(struct { int a; long b; })];
    __typeof__(struct { int t; }) typed;
    void (*callback)(struct { int x; }* arg);
    _Atomic struct {
        int value;
    } atomic;
    int tail;
};

struct §(inner_tags)Packed packed;

int width(struct Point* point) {
    return point->§(field_use)x;
}
