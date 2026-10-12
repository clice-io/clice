struct Plain {
    int value;
};

void build(Plain a) {
    Plain* p = §(new_kw)new Plain;
    Plain b §(copy_eq)= a;
}
