/// - verify: server

struct Vec {
    int x;
};

Vec operator+(Vec left, Vec right);

Vec §(op_def)operator+(Vec left, Vec right) {
    return Vec{left.x + right.x};
}

Vec combine(Vec left, Vec right) {
    return left §(op_use)+ right;
}
