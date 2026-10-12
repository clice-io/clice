namespace shapes {

struct §(base)Shape {};

struct Circle : Shape {
    struct §(nested)Arc : Shape {};
};

}  // namespace shapes
