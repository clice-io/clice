export module m;

#ifdef NDEBUG
export constexpr bool checked = false;
#else
export constexpr bool checked = true;
#endif
