#define SNAP_FLAG 1
#if __has_include(<snap§(angled)>)
#endif
#if __has_include("snap§(quoted)")
#endif
#if defined(SNAP_OTHER)
#elif defined(SNAP_FLAG) && __has_include_next("snap§(elif_next)")
#endif
#if defined(SNAP_FLAG) && \
    __has_include(<snap§(continued)>)
#endif
#if __has_include(<snap§(unclosed)) && __has_include(<snap_header.h>)
#endif
#if __has_include(<snap_header.h>) && __has_include(<snap§(second)>)
#endif
#if __has_include(<snap_header.h>) && SNAP_§(after_name)
#endif
#define HAS_SNAP __has_include(<snap§(in_define)>)
