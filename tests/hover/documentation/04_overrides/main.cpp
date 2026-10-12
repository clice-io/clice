namespace inherit_docs {
struct Base {
    /// Renders the widget.
    virtual void draw();
};
struct Circle : Base {
    void §(override)draw() override;
};
}
