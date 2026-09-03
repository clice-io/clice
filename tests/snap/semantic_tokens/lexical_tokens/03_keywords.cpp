/// # Keywords — including alternative operator spellings and the contextual `final` / `override`
///
/// - status: supported

bool logic(bool a, bool b) {
    return a §and b §or §not a;
}

struct Base {
    virtual void act();
    virtual ~Base();
};

struct Leaf §final : Base {
    void act() §override;
};

struct Last : Base {
    void act() §final;
};
