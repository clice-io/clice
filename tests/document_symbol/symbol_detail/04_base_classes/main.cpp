struct Shape {};
struct Tagged {};

struct Circle : Shape {
    double radius;
};

class Square : public Shape, private Tagged {};

struct Shared : virtual Shape {};
