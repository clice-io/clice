// C calls and initializers hint as C++ ones do.
struct point {
    int x;
    int y;
};

void draw(int width, int height);

struct file_ops {
    int (*read)(char* buffer, int size);
};

void use(struct file_ops* ops, char* buffer) {
    struct point p = {1, 2};
    draw(10, 20);
    ops->read(buffer, 64);
}
