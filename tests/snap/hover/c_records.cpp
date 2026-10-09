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
