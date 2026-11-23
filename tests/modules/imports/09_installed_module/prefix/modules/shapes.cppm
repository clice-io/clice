module;

#include <shapes/detail.h>

export module shapes;

export namespace shapes {

int area(int width, int height) {
    return detail::product(width, height);
}

}  // namespace shapes
