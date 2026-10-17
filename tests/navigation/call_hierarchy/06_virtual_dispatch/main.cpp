struct Base {
    virtual void §(base)draw();
};

struct Derived : Base {
    void §(override)draw() override;
};

void call_base(Base& b) {
    b.draw();
}

void call_derived(Derived& d) {
    d.draw();
}
