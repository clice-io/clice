// C designates fields in any order: the fields not designated yet are
// offered, in declaration order.
struct config {
    int width;
    int height;
    int fullscreen;
};

struct config first = { .height = 600, .§(after_height) };
