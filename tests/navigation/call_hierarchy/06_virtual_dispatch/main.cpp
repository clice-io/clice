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

#define TRACE(call) call
#define DRAW_BASE() Base::draw()

struct Logged : Base {
    void §(logged)draw() override {
        TRACE(Base::draw());
        DRAW_BASE();
    }
};
