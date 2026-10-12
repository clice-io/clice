struct Base {
    virtual int a() = 0;
    virtual int b() = 0;
    virtual int c() = 0;
};

struct Middle : Base {
    int b() override { return 1; }
};

class §(leaf)Leaf : public Middle {
    int c() override { return 2; }
};
