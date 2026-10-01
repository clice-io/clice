/// # Definitions using what lies between
///
/// - status: supported
///
/// A definition stays where it is when moving it would put it before something it uses, such as a variable or macro defined between the definitions; the others are reordered around it

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
