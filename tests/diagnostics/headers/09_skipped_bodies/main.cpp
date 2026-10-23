#include "shapes.h"

struct Square {
    int side;
};

int use() {
    return area(Square{2}) + unit();
}
