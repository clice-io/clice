/// # Find References
///
/// ## Macro references across expansions, `#ifdef`/`#ifndef` and `#undef`
///
/// - status: supported
/// - verify: server
/// - order: 9
///
/// A macro's references span its expansions, the `#ifdef` / `#ifndef`
/// conditionals that test it and the `#undef` that cancels it. Each
/// `#define` of a name is its own symbol, so a redefinition after `#undef`
/// collects only its own uses.

#define §(first)FEATURE 1

int on = FEATURE;

#ifdef FEATURE
int guarded = 1;
#endif

#ifndef FEATURE
int missing = 0;
#endif

#undef FEATURE

#define §(second)FEATURE 2

int again = FEATURE;
