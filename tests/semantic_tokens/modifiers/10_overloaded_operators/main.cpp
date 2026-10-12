struct Vec {
    Vec operator+(const Vec& other) const;
};

Vec add(Vec a, Vec b) {
    return a §(overloaded)+ b;
}

int add(int a, int b) {
    return a §(builtin)+ b;
}
