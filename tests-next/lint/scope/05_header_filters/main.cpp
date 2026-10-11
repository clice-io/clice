#include <platform.h>
#include "private.h"
#include "public.h"

double ratio(int done, int total) {
    return public_ratio(done, total) + private_ratio(done, total) + platform_ratio(done, total);
}
