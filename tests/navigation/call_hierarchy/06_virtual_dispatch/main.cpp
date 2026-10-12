struct Base {
    virtual void §(base)draw();
};

struct Derived : Base {
    void §(override)draw() override;
};

void call_derived(Derived& d) {
    d.draw();
}
