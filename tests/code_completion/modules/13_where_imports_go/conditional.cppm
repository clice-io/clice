#ifdef OTHER_BUILD
export module other;
#else
export module app:extra;
#endif

import :§(conditional_declaration)
