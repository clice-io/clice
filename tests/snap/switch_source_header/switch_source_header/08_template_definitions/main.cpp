/// # Template definitions apart
///
/// - status: supported
/// - verify: server
/// - indexing: true
///
/// A header and the `.tpp` file it includes for its template definitions switch to each other

// switch: matrix.h
// switch: matrix.tpp

#include "matrix.h"

int trace(Matrix<int>& matrix) {
    return matrix.at(0, 0) + matrix.at(1, 1);
}
