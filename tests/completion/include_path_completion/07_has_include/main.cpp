// Inside `__has_include` the header candidates of an include do not
// complete yet.
#if __has_include(<snap§(angled)>)
#endif
#if __has_include("snap§(quoted)")
#endif
