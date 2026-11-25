#include "types.h"
#include "utils.h"
#include "a.h"
#include "shapes.h"

int main() {
    return get_x({1, 2}) + shared_fn() + area({3, 4});
}
