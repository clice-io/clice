#if __has_include("header.h")
#include "header.h"
#endif

#if __has_embed("data.bin")
const char data[] = {
#embed "data.bin"
};
#endif
