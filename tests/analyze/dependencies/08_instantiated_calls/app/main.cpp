#include "core/clamp.h"
#include "core/scale.h"
#include "core/shape_convert.h"
#include "util/clamped.h"
#include "util/converted.h"
#include "util/made.h"
#include "util/scaled.h"
#include "util/total.h"

int main() {
    return total(Shape{3}) + scaled(Shape{4}) + clamped(Shape{5}) + made(Shape{6}) +
           converted(Shape{7});
}
