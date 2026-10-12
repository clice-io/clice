// An override declared in a class template implements the base method like
// any other.

struct Shape {
    virtual int §(base)area() const = 0;
};

template <typename T>
struct Scaled : Shape {
    int area() const override;
};

struct Square : Scaled<int> {
    int area() const override;
};
