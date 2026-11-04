#include "units.h"

struct Meters {
    double value;
};

static int meters_seen = 0;

#define METERS_SCALE 3

#include "ruler.h"

int main() {
    return ruler().value > 0 ? meters_seen : 1;
}
