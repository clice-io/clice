// A C struct completes its fields, and nothing a C++ class has implicitly.
struct point {
    int x;
    int y;
};

int sum(struct point p) {
    return p.§(member);
}
