// -Werror makes the header's unused variable an error, still the header's
// own; its redefinition and that warning's note stay out of main.cpp, the
// note not attached to the error before it either.
int x = undeclared;
#include "redef.h"
