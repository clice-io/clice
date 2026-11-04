#include "shapes.h"

struct Square {
    int side;
};

int total() {
    return area(Square{2});
}
