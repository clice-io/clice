// A guard's definition continued on a second line goes as a whole, and its
// macro stays defined for the program that reads it.
#ifndef CORE_SIZE_H
#define CORE_SIZE_H \
    1

namespace core {

inline int unit() {
    return 1;
}

}  // namespace core

#endif
