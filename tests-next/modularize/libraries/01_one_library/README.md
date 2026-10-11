# Wrap a library

A library's headers become a module interface unit that includes them and exports the names they declare, with the program's includes left as they are

The program still includes `<alpha/alpha.h>`: with `wrap/mirror/alpha`
first on its include path the header reads empty, and `wrap/prelude.h`,
force-included, imports `alpha` and defines the macros its headers did.
