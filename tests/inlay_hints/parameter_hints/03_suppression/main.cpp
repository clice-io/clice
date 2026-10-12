void draw(int width, int height);

void use() {
    int width = 5;
    int h = 2;
    draw(width, h);
    draw(/*width=*/1, /*height=*/2);
    draw(/*margin=*/6, 7);
}

struct Sizes {
    static int width;
    int height;

    void member() {
        draw(5, height);
    }
};

void qualified(Sizes s) {
    draw(Sizes::width, 3);
    draw(4, s.height);
}
