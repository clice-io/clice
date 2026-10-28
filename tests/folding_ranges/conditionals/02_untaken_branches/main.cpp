#ifdef _WIN32
int open_windows_handle();
#else
int open_posix_handle();
#endif

#if 0
#ifdef NESTED
int nested_in_untaken;
#endif
int untaken;
#endif
