namespace unnamed_alias_targets {
typedef struct {
    int x;
    int y;
} §(typedef_struct)Point;

using §(using_struct)Size = struct {
    int width;
    int height;
};

typedef enum { Red, Green } §(typedef_enum)Color;

struct §(members)Shape {
    typedef struct {
        int first;
        int second;
    } Pair;
    using Extent = struct {
        long length;
    };
    Pair corners;
    Extent extent;
};
}
