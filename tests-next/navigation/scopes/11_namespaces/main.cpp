// A namespace opened in several blocks is declared by each of them.

namespace §(first)geo {
int area();
}

namespace geo {
int perimeter();
}

int total() {
    return §(qualifier)geo::area() + geo::perimeter();
}
