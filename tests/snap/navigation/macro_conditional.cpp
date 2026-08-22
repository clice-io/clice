/// - verify: server
///
/// Conditional directives reference the macro like expansions do, while
/// builtin macros have no definition anywhere the index can serve.

#define §(guard_def)FEATURE_ON

#ifdef §(ifdef_use)FEATURE_ON
int enabled = 1;
#endif

#ifndef §(ifndef_use)FEATURE_ON
int disabled = 0;
#endif

int line = §(builtin_use)__LINE__;
