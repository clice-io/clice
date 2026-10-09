template <typename T>
T Matrix<T>::at(int row, int column) const {
    return cells[row * 2 + column];
}

template <typename T>
void Matrix<T>::set(int row, int column, T value) {
    cells[row * 2 + column] = value;
}
