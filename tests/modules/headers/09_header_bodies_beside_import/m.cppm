module;
#include "stream.h"
export module m;

export inline int use_stream() {
    Stream stream;
    return consume(stream);
}
