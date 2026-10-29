#pragma once

template <typename T>
class Matrix {
public:
    T at(int row, int column) const;
    void set(int row, int column, T value);

private:
    T cells[4] = {};
};

#include "matrix.tpp"
