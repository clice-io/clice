namespace shapes {

struct Circle {
    double radius;
    double area() const;
    bool operator==(const Circle& other) const;
};

enum class Kind { Round };

}  // namespace shapes
