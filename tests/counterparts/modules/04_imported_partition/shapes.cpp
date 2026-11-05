module shapes;

import :detail;

int scale(int value) {
    return value * 2;
}

int area(int side) {
    return scale(side * side);
}
