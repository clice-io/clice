/// # Macro references
///
/// - status: supported
/// - verify: server
///
/// Macro reference searches include expansions, conditional tests and undefinitions
///
/// Each `#define` of a name is its own symbol, so a redefinition after
/// `#undef` collects only its own uses.

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
