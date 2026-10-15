#pragma once

struct Square {
    int side;
};

inline int area(Square square) {
    return square.side * square.side;
}
