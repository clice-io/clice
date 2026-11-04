// A class template's member is named as the class declares it, whether or
// not the compile has instantiated its definition, which names it otherwise;
// an explicit specialization of it as it declares itself.
#include "text.h"

void use(Text<char>& text, Text<int>& numbers) {
    text.resize(§(uninstantiated)1);
    text.reserve(§(instantiated)1);
    numbers.append(§(specialized)1);
}
