// A class template's member is named as the class declares it, whether or
// not the compile has instantiated its definition, which names it otherwise.
#include "text.h"

void use(Text<char>& text) {
    text.resize(§(uninstantiated)1);
    text.reserve(§(instantiated)1);
}
