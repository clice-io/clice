bool logic(bool a, bool b) {
    return a §(and)and b §(or)or §(not)not a;
}

struct Base {
    virtual void act();
    virtual ~Base();
};

struct Leaf §(final_use)final : Base {
    void act() §(override)override;
};

struct Last : Base {
    void act() §(final_use2)final;
};
