// Macros in the leading run of directives, which the server compiles into
// its preamble, outline like the rest.

#define PREAMBLE_LIMIT 8
#define PREAMBLE_CHECK(cond) (!!(cond))

int after = PREAMBLE_LIMIT;
