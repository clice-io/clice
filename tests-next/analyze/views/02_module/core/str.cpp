#include "core/detail.h"
#include "core/sink.h"
#include "core/str.h"

int length(const Str& s) {
    return s.n + detail_value() + Sink{2}.fd;
}
