#include "matrix.h"

int trace(Matrix<int>& matrix) {
    return matrix.at(0, 0) + matrix.at(1, 1);
}
