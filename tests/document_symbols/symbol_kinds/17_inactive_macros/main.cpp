// Only the defines the preprocessor reads outline, not those in an inactive
// branch: in the leading run of directives, which the server compiles into
// its preamble, as after it.

#if 0
#define PREAMBLE_SKIPPED 1
#else
#define PREAMBLE_TAKEN 2
#endif

int after = PREAMBLE_TAKEN;

#ifdef UNDEFINED_FLAG
#define BODY_SKIPPED 3
#elif 1
#define BODY_TAKEN 4
#endif
