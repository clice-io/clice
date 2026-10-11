// An include spelled with angle brackets and found through -I opens the
// header as a quoted include does.

#include §(angled)<shapes/circle.h>

double probe() {
    return area(1.0);
}
