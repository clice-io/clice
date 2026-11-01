#pragma once
#if defined(APP_BACKEND)
inline const char* backend() { return "app"; }
#else
inline const char* backend() { return "lib"; }
#endif
