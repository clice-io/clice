void resize(int, int);

void fill(int _value, int __count);

int scale(int good);

void blend(int alpha, int beta);
void blend(int, int);

void use() {
    resize(800, 600);
    fill(1, 2);
    scale(7);
    blend(1, 2);
}

void blend(int, int) {}

void resize(int width, int height) {}

int scale(int bad) {
    return bad;
}
