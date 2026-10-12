int anchor = 0;

#define §(define_site)LIMIT 64

int use = §(use_site)LIMIT;

#ifdef §(ifdef_site)LIMIT
int guarded = 1;
#endif

#if defined(§(defined_site)LIMIT)
int tested = 1;
#endif

#undef §(undef_site)LIMIT
