// C designates a field again for another part of it: a field designated in
// part (`.origin.x = 1`, `.items[0] = 1`) is still offered, one designated
// whole is not.
struct point {
    int x;
    int y;
};

struct shape {
    int id;
    struct point origin;
    int items[4];
};

struct shape first = { .origin.x = 1, .items[0] = 1, .§(after_part) };
struct shape second = { .origin = {0}, .§(after_whole) };
