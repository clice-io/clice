/// # Definition text at every site
///
/// - status: supported
///
/// `#define`, use, `#ifdef` and `#undef` all show the macro's definition
///
/// A macro's hover card carries its `#define` text wherever the name
/// appears: the definition itself, a use, an `#ifdef` guard and an `#undef`.

int anchor = 0;

#define §(01_define_site)LIMIT 64

int use = §(02_use_site)LIMIT;

#ifdef §(03_ifdef_site)LIMIT
int guarded = 1;
#endif

#undef §(04_undef_site)LIMIT
