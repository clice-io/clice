struct Base {
    virtual void §(base)run() = 0;
};

struct Middle : Base {
    void §(middle)run() override {}
};

struct Leaf : Middle {
    void run() override {}
};
