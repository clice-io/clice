// C calls and initializers hint as C++ ones do.
struct point {
    int x;
    int y;
};

void draw(int width, int height);

void use(void) {
    struct point p = {1, 2};
    draw(10, 20);
}
