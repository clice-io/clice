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

struct §(flexible)Message {
    int length;
    char §(flexible_member)text[];
};

// C records: a struct's card lists its members, anonymous unions and
// structs included, and tags first declared inside other declarations.
