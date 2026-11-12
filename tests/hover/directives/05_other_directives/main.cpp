#define SETTINGS_HEADER "settings.h"
#include SETTINGS_§(macro_include)HEADER

#if __has_include("sett§(has_include)ings.h")
#endif

const unsigned char data[] = {
#embed "da§(embed)ta.bin"
};

#if __has_embed("da§(has_embed)ta.bin")
#endif
