struct §(cls)S {
    int a();
    int b();
    int c();
    int d();
};

int S::d() {
    return 4;
}

static int counter = 0;

int S::c() {
    return counter;
}

#define LIMIT 2

int S::b() {
    return LIMIT;
}

int S::a() {
    return 1;
}
