#include "core/clamp.h"
#include "core/scale.h"
#include "util/clamped.h"
#include "util/scaled.h"
#include "util/total.h"

int main() {
    return total(Shape{3}) + scaled(Shape{4}) + clamped(Shape{5});
}
