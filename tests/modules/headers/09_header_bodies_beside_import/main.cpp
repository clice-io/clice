// stream.h opens the preamble, and m's global module fragment includes it
// too: the preamble keeps the bodies of its headers' functions, which
// clang compares with the module's.
#include "stream.h"
import m;

int main() {
    Stream stream;
    return stream.get<int>() + use_stream();
}
