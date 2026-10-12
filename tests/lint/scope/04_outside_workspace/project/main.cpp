// Pins the workspace boundary: a unit the database lists outside the
// workspace is not checked, and neither is a header outside it, whether the
// build includes it as a system header or not.
#include <platform.h>
#include "shared.h"

double ratio(int done, int total) {
    return shared_ratio(done, total) + platform_ratio(done, total) + done / total;
}
