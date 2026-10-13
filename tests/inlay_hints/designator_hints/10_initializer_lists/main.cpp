// The elements of a `std::initializer_list` have no designators to write,
// array indices turned on or not.
#include <initializer_list>

struct Numbers {
    Numbers(std::initializer_list<int> values);
};

void take(std::initializer_list<int> values);

Numbers numbers{1, 2, 3};
int plain[2] = {6, 7};

void use() {
    take({4, 5});
}
