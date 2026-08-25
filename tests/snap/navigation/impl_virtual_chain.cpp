/// # Go to Implementation
///
/// ## Virtual methods — each level of a chain to its own overriders
///
/// - status: supported
/// - verify: server
/// - order: 1
///
/// Along a three-level override chain, go-to-implementation from each method
/// reaches the override one level down — base to middle, middle to leaf.

struct Base {
    virtual void §(base)run() = 0;
};

struct Middle : Base {
    void §(middle)run() override {}
};

struct Leaf : Middle {
    void run() override {}
};
