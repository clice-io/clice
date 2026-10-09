/// # Override chain
///
/// - status: supported
/// - verify: server
///
/// Implementation navigation follows an override chain one level at a time
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
