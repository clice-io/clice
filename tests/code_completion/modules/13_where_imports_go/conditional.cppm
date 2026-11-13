// The module is declared under a conditional, which the preprocessor reads
// with the file's command: its own module's partitions follow.
#ifdef OTHER_BUILD
export module other;
#else
export module app:extra;
#endif

import :§(conditional_declaration)
