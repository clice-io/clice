struct Base {
    virtual void §(base)draw();
};

struct Derived : Base {
    void §(override)draw() override;
};

void Derived::draw() {
    Base::draw();
}

void call_base(Base& b) {
    b.draw();
}

void call_derived(Derived& d) {
    d.draw();
}

struct Group : Base {
    Base* child;

    void draw() override {
        Base::draw();
        child->draw();
    }
};
