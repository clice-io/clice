/// # Keywords
///
/// - status: supported
///
/// Alternative operator spellings and contextual specifiers retain keyword
/// tokens

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
