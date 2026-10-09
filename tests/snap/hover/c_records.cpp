// - flags: ["-x", "c", "-std=c11"]

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

struct §(c_struct)Point point;

struct ops {
    int (*open)(struct inode*, struct file*);
    const struct item* first;
    int count;
};

struct §(c_mentions)ops table;

struct Packed {
    char raw[sizeof(struct { int a; long b; })];
    __typeof__(struct { int t; }) typed;
    void (*callback)(struct { int x; }* arg);
    _Atomic struct {
        int value;
    } atomic;
    int tail;
};

struct §(c_inner_tags)Packed packed;
