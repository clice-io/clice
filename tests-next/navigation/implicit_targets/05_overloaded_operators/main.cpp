struct Iterator {
    int value;
};

struct Vec {
    Vec operator+(const Vec& other) const;
    int operator[](int index) const;
    int operator()(int a, int b) const;
    Iterator* operator->();
};

void use(Vec a, Vec b) {
    Vec c = a §(plus)+ b;
    int e = a§(subscript)[0];
    int f = a§(call)(1, 2);
    a§(arrow)->value;
}
