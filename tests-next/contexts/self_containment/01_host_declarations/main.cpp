struct Meters {
    double value;
};

#define SCALE 3

#include "ruler.h"

int main() {
    return ruler_length().value > unit_length.value ? 0 : 1;
}
