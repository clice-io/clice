void blend(int alpha, int beta);
void blend(int, int);

void shade(int light);
void shade(int);

void use() {
    blend(1, 2);
    shade(3);
}

void blend(int, int) {}

void shade(int dark) {}
