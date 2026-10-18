struct Shape {
    virtual int draw(int x, int y) const;
    virtual void erase();
    virtual void reset() = delete;
};

struct Polygon : Shape {};

struct Triangle : Polygon {
    dr§(indirect)
};

struct Square : Polygon {
    int draw(int x, int y) const override;
    void erase() final;
};

struct Cube : Square {
    dr§(overridden_between)
};

struct Prism : Square {
    eras§(final_between)
};

struct Circle : Polygon {
    rese§(deleted)
};
