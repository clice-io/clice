struct Base {
    virtual void draw();
    virtual void paint();
};

struct Derived : Base {
    void draw() §(override)override;
    void paint() §(final)final;
};
