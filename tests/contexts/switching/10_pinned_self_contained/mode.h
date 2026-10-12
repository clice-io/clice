#pragma once
#ifdef FAST
inline int mode() { return 1; }
#else
inline int mode() { return 2; }
#endif
